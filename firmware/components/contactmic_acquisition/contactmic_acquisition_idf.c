#include "contactmic_acquisition_idf.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

#define CM_ACQ_TASK_STACK 4096u
#define CM_ACQ_MEASURE_US 250000ull
#define CM_ACQ_TASK_WAIT_MS 10u

struct cm_acq_idf {
    cm_acq core;
    cmq_device *device;
    TaskHandle_t task;
    SemaphoreHandle_t gate;
    cm_acq_rate_window rate_window;
    uint64_t rate_deadline_us;
    uint32_t running;
    uint32_t rate_active;
    uint32_t measurement_done;
    cm_acq_policy policy;
    cm_acq_mode mode;
    cm_acq_mode auto_ceiling;
    cmq_axis axis;
    uint8_t range_g;
    bool configured;
    bool isr_added;
    bool owns_isr_service;
};

static uint32_t load_flag(const uint32_t *flag)
{
    return __atomic_load_n(flag, __ATOMIC_ACQUIRE);
}

static void store_flag(uint32_t *flag, uint32_t value)
{
    __atomic_store_n(flag, value, __ATOMIC_RELEASE);
}

static esp_err_t map_qmi(cmq_result result)
{
    switch (result) {
    case CMQ_OK: return ESP_OK;
    case CMQ_INVALID: return ESP_ERR_INVALID_ARG;
    case CMQ_STATE: return ESP_ERR_INVALID_STATE;
    case CMQ_NOT_FOUND: return ESP_ERR_NOT_FOUND;
    case CMQ_IO: return ESP_FAIL;
    default: return ESP_FAIL;
    }
}

static cmq_mode qmi_mode(cm_acq_mode mode)
{
    switch (mode) {
    case CM_ACQ_MODE_HIGH: return CMQ_HIGH;
    case CM_ACQ_MODE_MEDIUM: return CMQ_MEDIUM;
    default: return CMQ_SAFE;
    }
}

static cm_acq_read_result read_selected(void *opaque, int16_t *sample)
{
    cm_acq_idf *ctx = (cm_acq_idf *)opaque;
    cmq_result result = cmq_read_selected(ctx->device, sample);
    if (result == CMQ_OK) return CM_ACQ_READ_OK;
    if (result == CMQ_IO) return CM_ACQ_READ_IO;
    return CM_ACQ_READ_FAULT;
}

static void drdy_isr(void *opaque)
{
    cm_acq_idf *ctx = (cm_acq_idf *)opaque;
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(ctx->task, &wake);
    if (wake == pdTRUE) portYIELD_FROM_ISR();
}

static void acquisition_task(void *opaque)
{
    cm_acq_idf *ctx = (cm_acq_idf *)opaque;
    for (;;) {
        uint32_t edges = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CM_ACQ_TASK_WAIT_MS));
        if (edges != 0u) {
            xSemaphoreTake(ctx->gate, portMAX_DELAY);
            if (load_flag(&ctx->running)) {
                (void)cm_acq_service_edges(&ctx->core, edges);
            }
            xSemaphoreGive(ctx->gate);
        }

        if (load_flag(&ctx->running) && load_flag(&ctx->rate_active) &&
            (uint64_t)esp_timer_get_time() >= ctx->rate_deadline_us) {
            xSemaphoreTake(ctx->gate, portMAX_DELAY);
            if (load_flag(&ctx->running) && load_flag(&ctx->rate_active)) {
                (void)cm_acq_rate_finish(&ctx->core, &ctx->rate_window,
                                         (uint64_t)esp_timer_get_time());
                store_flag(&ctx->rate_active, 0u);
                store_flag(&ctx->measurement_done, 1u);
            }
            xSemaphoreGive(ctx->gate);
        }
    }
}

static esp_err_t configure_mode(cm_acq_idf *ctx, cm_acq_mode mode)
{
    /* A failed register transaction leaves CM-002 in FAULT; do not retain a
     * stale software claim that this adapter is configured. */
    ctx->configured = false;
    cmq_config config = {
        .axis = ctx->axis,
        .mode = qmi_mode(mode),
        .range_g = ctx->range_g,
        .drdy_enabled = true,
    };
    cmq_result result = cmq_configure(ctx->device, &config);
    if (result != CMQ_OK) return map_qmi(result);
    ctx->mode = mode;
    ctx->configured = true;
    return ESP_OK;
}

esp_err_t cm_acq_idf_create(cmq_device *device, cm_acq_idf **out)
{
    if (!device || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    cm_acq_idf *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return ESP_ERR_NO_MEM;
    ctx->device = device;
    if (cm_acq_init(&ctx->core, read_selected, ctx) != CM_ACQ_OK) {
        free(ctx);
        return ESP_FAIL;
    }

    ctx->gate = xSemaphoreCreateMutex();
    if (!ctx->gate) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t gpio = {
        .pin_bit_mask = 1ull << CMQ_INT2_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&gpio);
    if (err != ESP_OK) goto fail;

    if (xTaskCreate(acquisition_task, "contactmic_acq", CM_ACQ_TASK_STACK, ctx,
                    configMAX_PRIORITIES - 2, &ctx->task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    err = gpio_install_isr_service(0);
    if (err == ESP_OK) {
        ctx->owns_isr_service = true;
    } else if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    } else {
        goto fail;
    }

    err = gpio_isr_handler_add((gpio_num_t)CMQ_INT2_GPIO, drdy_isr, ctx);
    if (err != ESP_OK) goto fail;
    ctx->isr_added = true;
    (void)gpio_intr_disable((gpio_num_t)CMQ_INT2_GPIO);

    *out = ctx;
    return ESP_OK;

fail:
    if (ctx->isr_added) (void)gpio_isr_handler_remove((gpio_num_t)CMQ_INT2_GPIO);
    if (ctx->owns_isr_service) gpio_uninstall_isr_service();
    if (ctx->task) vTaskDelete(ctx->task);
    if (ctx->gate) vSemaphoreDelete(ctx->gate);
    free(ctx);
    return err;
}

void cm_acq_idf_destroy(cm_acq_idf *ctx)
{
    if (!ctx) return;
    (void)cm_acq_idf_stop(ctx);
    (void)gpio_intr_disable((gpio_num_t)CMQ_INT2_GPIO);
    if (ctx->isr_added) (void)gpio_isr_handler_remove((gpio_num_t)CMQ_INT2_GPIO);
    if (ctx->task) vTaskDelete(ctx->task);
    if (ctx->owns_isr_service) gpio_uninstall_isr_service();
    if (ctx->gate) vSemaphoreDelete(ctx->gate);
    free(ctx);
}

esp_err_t cm_acq_idf_configure(cm_acq_idf *ctx, cm_acq_policy policy,
                               cmq_axis axis, uint8_t range_g,
                               cm_acq_mode auto_ceiling)
{
    if (!ctx || axis < CMQ_X || axis > CMQ_Z || (range_g != 2u && range_g != 4u))
        return ESP_ERR_INVALID_ARG;
    if (load_flag(&ctx->running)) return ESP_ERR_INVALID_STATE;

    cm_acq_mode selected;
    if (!cm_acq_policy_initial(policy, auto_ceiling, &selected))
        return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(ctx->gate, portMAX_DELAY);
    ctx->policy = policy;
    ctx->auto_ceiling = auto_ceiling;
    ctx->axis = axis;
    ctx->range_g = range_g;
    esp_err_t err = configure_mode(ctx, selected);
    xSemaphoreGive(ctx->gate);
    return err;
}

esp_err_t cm_acq_idf_start(cm_acq_idf *ctx)
{
    if (!ctx || !ctx->configured) return ESP_ERR_INVALID_STATE;
    if (load_flag(&ctx->running)) return ESP_ERR_INVALID_STATE;

    (void)gpio_intr_disable((gpio_num_t)CMQ_INT2_GPIO);
    store_flag(&ctx->running, 0u);
    store_flag(&ctx->rate_active, 0u);
    store_flag(&ctx->measurement_done, 0u);
    (void)ulTaskNotifyValueClear(ctx->task, UINT32_MAX);

    xSemaphoreTake(ctx->gate, portMAX_DELAY);
    cmq_result result = cmq_start(ctx->device);
    if (result != CMQ_OK) {
        xSemaphoreGive(ctx->gate);
        return map_qmi(result);
    }
    if (cm_acq_start_session(&ctx->core) != CM_ACQ_OK) {
        (void)cmq_stop(ctx->device);
        xSemaphoreGive(ctx->gate);
        return ESP_FAIL;
    }

    uint64_t now = (uint64_t)esp_timer_get_time();
    if (!cm_acq_rate_begin(&ctx->core, now, &ctx->rate_window)) {
        (void)cmq_stop(ctx->device);
        (void)cm_acq_stop_session(&ctx->core);
        xSemaphoreGive(ctx->gate);
        return ESP_FAIL;
    }
    ctx->rate_deadline_us = now + CM_ACQ_MEASURE_US;
    store_flag(&ctx->rate_active, 1u);
    store_flag(&ctx->running, 1u);
    (void)ulTaskNotifyValueClear(ctx->task, UINT32_MAX);
    xSemaphoreGive(ctx->gate);

    esp_err_t err = gpio_intr_enable((gpio_num_t)CMQ_INT2_GPIO);
    if (err != ESP_OK) {
        (void)cm_acq_idf_stop(ctx);
        return err;
    }
    return ESP_OK;
}

esp_err_t cm_acq_idf_stop(cm_acq_idf *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    (void)gpio_intr_disable((gpio_num_t)CMQ_INT2_GPIO);
    store_flag(&ctx->running, 0u);
    store_flag(&ctx->rate_active, 0u);

    if (!ctx->gate) return ESP_OK;
    xSemaphoreTake(ctx->gate, portMAX_DELAY);
    (void)cm_acq_stop_session(&ctx->core);
    cmq_result result = CMQ_OK;
    if (ctx->device &&
        (ctx->device->state == CMQ_RUNNING || ctx->device->state == CMQ_CONFIGURED ||
         ctx->device->state == CMQ_FAULT)) {
        result = cmq_stop(ctx->device);
    }
    if (ctx->task) (void)ulTaskNotifyValueClear(ctx->task, UINT32_MAX);
    xSemaphoreGive(ctx->gate);
    return map_qmi(result);
}

esp_err_t cm_acq_idf_transition(cm_acq_idf *ctx, bool degrade,
                                cm_acq_mode transport_ceiling)
{
    if (!ctx || !ctx->configured) return ESP_ERR_INVALID_STATE;
    cm_acq_mode next;
    if (!cm_acq_policy_step(ctx->policy, ctx->mode, transport_ceiling, degrade, &next))
        return ESP_ERR_INVALID_ARG;

    bool was_running = load_flag(&ctx->running) != 0u;
    ctx->auto_ceiling = transport_ceiling;
    if (next == ctx->mode) return ESP_OK;

    if (was_running) {
        esp_err_t err = cm_acq_idf_stop(ctx);
        if (err != ESP_OK) return err;
    }

    xSemaphoreTake(ctx->gate, portMAX_DELAY);
    esp_err_t err = configure_mode(ctx, next);
    xSemaphoreGive(ctx->gate);
    if (err != ESP_OK) return err;

    return was_running ? cm_acq_idf_start(ctx) : ESP_OK;
}

bool cm_acq_idf_measurement_ready(const cm_acq_idf *ctx)
{
    return ctx && load_flag(&ctx->measurement_done) != 0u;
}

uint32_t cm_acq_idf_measured_rate_millihz(const cm_acq_idf *ctx)
{
    return ctx ? cm_acq_measured_rate_millihz(&ctx->core) : 0u;
}

cm_acq_mode cm_acq_idf_current_mode(const cm_acq_idf *ctx)
{
    return ctx ? ctx->mode : CM_ACQ_MODE_SAFE;
}

cm_acq *cm_acq_idf_core(cm_acq_idf *ctx)
{
    return ctx ? &ctx->core : NULL;
}
