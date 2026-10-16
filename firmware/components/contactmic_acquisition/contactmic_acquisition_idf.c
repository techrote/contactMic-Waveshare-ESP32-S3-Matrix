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
    uint64_t rate_started_us;
    uint64_t rate_deadline_us;
    uint32_t running;
    uint32_t rate_active;
    uint32_t measurement_done;
    uint32_t publication; /* 0 calibration, 1 commit requested, 2 producer live */
    portMUX_TYPE pending_lock;
    uint64_t pending_edges;
    uint64_t observed_edges;
    bool record_enabled;
    cm_acq_policy policy;
    cm_acq_mode mode;
    cm_acq_mode auto_ceiling;
    cmq_axis axis;
    uint8_t range_g;
    bool configured;
    bool isr_added;
    bool owns_isr_service;
    cm_acq_idf_timing timing;
};

static void diagnostic_add(uint32_t *p, uint32_t n)
{
    uint32_t v = __atomic_load_n(p, __ATOMIC_RELAXED);
    __atomic_store_n(p, UINT32_MAX - v < n ? UINT32_MAX : v + n, __ATOMIC_RELEASE);
}

static uint32_t load_flag(const uint32_t *flag)
{
    return __atomic_load_n(flag, __ATOMIC_ACQUIRE);
}

static void store_flag(uint32_t *flag, uint32_t value)
{
    __atomic_store_n(flag, value, __ATOMIC_RELEASE);
}

static void invalidate_measurement(cm_acq_idf *ctx)
{
    store_flag(&ctx->rate_active, 0u);
    store_flag(&ctx->measurement_done, 0u);
    __atomic_store_n(&ctx->core.measured_rate_millihz, 0u, __ATOMIC_RELEASE);
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

static bool read_window_open(cm_acq_idf *ctx, bool after)
{
    portENTER_CRITICAL(&ctx->pending_lock);
    bool pending = ctx->pending_edges != 0;
    bool high = gpio_get_level((gpio_num_t)CMQ_INT2_GPIO) != 0;
    portEXIT_CRITICAL(&ctx->pending_lock);
    /* Mutually exclusive per-check diagnostics; source_faults is still the
     * single failed-read count. Never add these as independent source losses. */
    if (pending) diagnostic_add(after ? &ctx->timing.post_pending : &ctx->timing.pre_pending, 1);
    else if (!high) diagnostic_add(after ? &ctx->timing.post_low : &ctx->timing.pre_low, 1);
    return !pending && high;
}

static cm_acq_read_result read_selected(void *opaque, int16_t *sample)
{
    cm_acq_idf *ctx = (cm_acq_idf *)opaque;
    /* CM-002 requires the DRDY-high window. Never publish an I2C-success value
     * after an observed intervening edge/update. Physical interrupt latency and
     * bus-window feasibility still require CM-007 measurement. */
    if (!read_window_open(ctx, false)) return CM_ACQ_READ_FAULT;
    int16_t value;
    uint64_t start = (uint64_t)esp_timer_get_time();
    cmq_result result = cmq_read_selected(ctx->device, &value);
    bool valid = result == CMQ_OK && read_window_open(ctx, true);
    uint64_t elapsed = (uint64_t)esp_timer_get_time() - start;
    uint32_t us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    diagnostic_add(&ctx->timing.read_calls, 1);
    diagnostic_add(&ctx->timing.read_us_total, us);
    if (!ctx->timing.read_us_min || us < ctx->timing.read_us_min)
        __atomic_store_n(&ctx->timing.read_us_min, us, __ATOMIC_RELEASE);
    if (us > ctx->timing.read_us_max)
        __atomic_store_n(&ctx->timing.read_us_max, us, __ATOMIC_RELEASE);
    if (result == CMQ_IO) return CM_ACQ_READ_IO;
    if (!valid) return CM_ACQ_READ_FAULT;
    *sample = value;
    return CM_ACQ_READ_OK;
}

static void drdy_isr(void *opaque)
{
    cm_acq_idf *ctx = (cm_acq_idf *)opaque;
    portENTER_CRITICAL_ISR(&ctx->pending_lock);
    bool record = ctx->record_enabled;
    if (record) {
        if (ctx->pending_edges != UINT64_MAX) ++ctx->pending_edges;
        if (ctx->observed_edges != UINT64_MAX) ++ctx->observed_edges;
    }
    portEXIT_CRITICAL_ISR(&ctx->pending_lock);
    if (!record) return;

    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(ctx->task, &wake);
    if (wake == pdTRUE) portYIELD_FROM_ISR();
}

static void clear_pending(cm_acq_idf *ctx)
{
    portENTER_CRITICAL(&ctx->pending_lock);
    ctx->pending_edges = 0u;
    portEXIT_CRITICAL(&ctx->pending_lock);
    if (ctx->task) (void)ulTaskNotifyValueClear(ctx->task, UINT32_MAX);
}

static uint64_t take_pending(cm_acq_idf *ctx)
{
    portENTER_CRITICAL(&ctx->pending_lock);
    uint64_t edges = ctx->pending_edges;
    ctx->pending_edges = 0u;
    portEXIT_CRITICAL(&ctx->pending_lock);
    return edges;
}

static void acquisition_cycle(cm_acq_idf *ctx)
{
    xSemaphoreTake(ctx->gate, portMAX_DELAY);
    uint64_t edges = take_pending(ctx);
    if (load_flag(&ctx->running)) {
        if (edges) (void)cm_acq_service_edges(&ctx->core, edges);
        uint32_t publication = load_flag(&ctx->publication);
        if (publication != 2u) {
            /* Until producer acknowledgment, this task is the sole consumer.
             * Calibration cannot fill the ring or become host audio. */
            (void)cm_acq_consume(&ctx->core, CM_ACQ_RING_CAPACITY);
            if (publication == 1u) {
                (void)cm_acq_rebase_publication(&ctx->core);
                store_flag(&ctx->publication, 2u);
            }
        }
        if (load_flag(&ctx->rate_active) &&
            (uint64_t)esp_timer_get_time() >= ctx->rate_deadline_us) {
            /* Count observed edges, including those pending during a slow I2C
             * transaction, rather than underreporting the rate as service lags. */
            portENTER_CRITICAL(&ctx->pending_lock);
            uint64_t count = ctx->observed_edges;
            uint64_t now = (uint64_t)esp_timer_get_time();
            portEXIT_CRITICAL(&ctx->pending_lock);
            uint32_t rate = 0;
            if (now > ctx->rate_started_us && count != UINT64_MAX)
                (void)cm_acq_calculate_rate(count, now - ctx->rate_started_us, &rate);
            __atomic_store_n(&ctx->core.measured_rate_millihz, rate, __ATOMIC_RELEASE);
            store_flag(&ctx->rate_active, 0u);
            store_flag(&ctx->measurement_done, 1u);
        }
    }
    xSemaphoreGive(ctx->gate);
}

static void acquisition_task(void *opaque)
{
    cm_acq_idf *ctx = (cm_acq_idf *)opaque;
    TickType_t delay = pdMS_TO_TICKS(CM_ACQ_TASK_WAIT_MS);
    if (!delay) delay = 1;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, delay);
        acquisition_cycle(ctx);
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
    portMUX_INITIALIZE(&ctx->pending_lock);
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
    /* A changed source configuration makes any prior session's measured rate
     * stale immediately, before a new start/measurement window exists. */
    invalidate_measurement(ctx);
    esp_err_t err = configure_mode(ctx, selected);
    xSemaphoreGive(ctx->gate);
    return err;
}

static esp_err_t start_mode(cm_acq_idf *ctx, bool calibration)
{
    if (!ctx || !ctx->configured) return ESP_ERR_INVALID_STATE;
    if (load_flag(&ctx->running)) return ESP_ERR_INVALID_STATE;

    (void)gpio_intr_disable((gpio_num_t)CMQ_INT2_GPIO);
    store_flag(&ctx->running, 0u);
    store_flag(&ctx->rate_active, 0u);
    store_flag(&ctx->measurement_done, 0u);
    clear_pending(ctx);

    xSemaphoreTake(ctx->gate, portMAX_DELAY);
    cmq_result result = cmq_start(ctx->device);
    if (result != CMQ_OK) {
        /* CM-002 start failures may latch FAULT. Require explicit recovery and
         * configuration rather than advertising stale readiness. */
        ctx->configured = false;
        xSemaphoreGive(ctx->gate);
        return map_qmi(result);
    }
    if (cm_acq_start_session(&ctx->core) != CM_ACQ_OK) {
        (void)cmq_stop(ctx->device);
        xSemaphoreGive(ctx->gate);
        return ESP_FAIL;
    }

    uint64_t now = (uint64_t)esp_timer_get_time();
    ctx->rate_started_us = now;
    ctx->rate_deadline_us = now + CM_ACQ_MEASURE_US;
    portENTER_CRITICAL(&ctx->pending_lock);
    ctx->observed_edges = 0;
    ctx->record_enabled = true;
    portEXIT_CRITICAL(&ctx->pending_lock);
    store_flag(&ctx->publication, calibration ? 0u : 2u);
    store_flag(&ctx->rate_active, 1u);
    store_flag(&ctx->running, 1u);
    clear_pending(ctx);
    xSemaphoreGive(ctx->gate);

    esp_err_t err = gpio_intr_enable((gpio_num_t)CMQ_INT2_GPIO);
    if (err != ESP_OK) {
        (void)cm_acq_idf_stop(ctx);
        return err;
    }
    return ESP_OK;
}

esp_err_t cm_acq_idf_start(cm_acq_idf *ctx) { return start_mode(ctx, false); }
esp_err_t cm_acq_idf_prepare(cm_acq_idf *ctx) { return start_mode(ctx, true); }
esp_err_t cm_acq_idf_publish(cm_acq_idf *ctx)
{
    if (!ctx || !load_flag(&ctx->running) || !load_flag(&ctx->measurement_done) ||
        !cm_acq_measured_rate_millihz(&ctx->core) || load_flag(&ctx->publication))
        return ESP_ERR_INVALID_STATE;
    store_flag(&ctx->publication, 1u);
    xTaskNotifyGive(ctx->task);
    return ESP_OK;
}
bool cm_acq_idf_publishing(const cm_acq_idf *ctx)
{ return ctx && load_flag(&ctx->publication) == 2u; }

esp_err_t cm_acq_idf_stop(cm_acq_idf *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    esp_err_t irq = gpio_intr_disable((gpio_num_t)CMQ_INT2_GPIO);
    if (irq != ESP_OK) return irq;
    portENTER_CRITICAL(&ctx->pending_lock);
    ctx->record_enabled = false;
    portEXIT_CRITICAL(&ctx->pending_lock);

    if (!ctx->gate) return ESP_OK;
    xSemaphoreTake(ctx->gate, portMAX_DELAY);
    bool was_running = load_flag(&ctx->running) != 0u;
    uint64_t pending = take_pending(ctx);
    if (was_running && pending != 0u) {
        /* Interrupts are disabled and no read will be attempted during stop.
         * Preserve every observed interval as explicit source loss before the
         * session-local index/ring are cleared. */
        (void)cm_acq_discard_edges(&ctx->core, pending);
    }
    store_flag(&ctx->running, 0u);
    invalidate_measurement(ctx);
    store_flag(&ctx->publication, 0u);
    (void)cm_acq_stop_session(&ctx->core);
    cmq_result result = CMQ_OK;
    if (ctx->device &&
        (ctx->device->state == CMQ_RUNNING || ctx->device->state == CMQ_CONFIGURED ||
         ctx->device->state == CMQ_FAULT)) {
        result = cmq_stop(ctx->device);
    }
    clear_pending(ctx);
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
    invalidate_measurement(ctx);
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

void cm_acq_idf_timing_snapshot(const cm_acq_idf *ctx, cm_acq_idf_timing *out)
{
    if (!ctx || !out) return;
#define COPY_TIMING(field) out->field = __atomic_load_n(&ctx->timing.field, __ATOMIC_ACQUIRE)
    COPY_TIMING(pre_low); COPY_TIMING(pre_pending); COPY_TIMING(post_low); COPY_TIMING(post_pending);
    COPY_TIMING(read_calls); COPY_TIMING(read_us_min); COPY_TIMING(read_us_max); COPY_TIMING(read_us_total);
#undef COPY_TIMING
}

esp_err_t cm_acq_idf_zero_rate_probe(cm_acq_idf *ctx, uint32_t out[8])
{
    if (!ctx || !out) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->gate, portMAX_DELAY);
    esp_err_t rc = ESP_ERR_INVALID_STATE;
    if (load_flag(&ctx->measurement_done) && !cm_acq_measured_rate_millihz(&ctx->core)
        && load_flag(&ctx->running) && !load_flag(&ctx->publication)) {
        uint8_t bytes[24];
        gpio_io_config_t io;
        uint32_t levels = 0;
        rc = map_qmi(cmq_diagnostic_snapshot(ctx->device, bytes));
        if (rc == ESP_OK) {
            rc = gpio_get_io_config((gpio_num_t)CMQ_INT2_GPIO, &io);
        }
        if (rc == ESP_OK) {
            /* Failure-only bounded polling, never an acquisition substitute.
             * IRQs remain enabled. USB/higher-priority tasks can preempt us. */
            uint64_t end = (uint64_t)esp_timer_get_time() + 20000u;
            do {
                levels |= gpio_get_level((gpio_num_t)CMQ_INT2_GPIO) ? 2u : 1u;
            } while ((uint64_t)esp_timer_get_time() < end);
            rc = map_qmi(cmq_diagnostic_snapshot(ctx->device, bytes + 12));
        }
        if (rc == ESP_OK) {
            for (size_t i = 0; i < 6; ++i)
                out[i] = ((uint32_t)bytes[4*i] << 24) | ((uint32_t)bytes[4*i+1] << 16)
                    | ((uint32_t)bytes[4*i+2] << 8) | bytes[4*i+3];
            out[6] = ((uint32_t)io.fun_sel << 16) | (io.ie ? 1u : 0u)
                | (io.oe ? 2u : 0u) | (io.pu ? 4u : 0u) | (io.pd ? 8u : 0u);
            out[7] = levels;
        }
    }
    xSemaphoreGive(ctx->gate);
    return rc;
}
