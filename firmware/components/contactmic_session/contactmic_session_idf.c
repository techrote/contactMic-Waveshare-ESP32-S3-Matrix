#include "contactmic_session_idf.h"
#include "contactmic_acquisition_idf.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <inttypes.h>

/* Fixed boot-lifetime ownership, including on initialization failure. No retry
 * loop allocates more contexts, and no task/context is freed behind an ISR. */
static struct {
    cm_session session;
    cm_acq_idf *acquisition;
    cmq_device *device;
    bool claimed;
    uint32_t boundary_event; /* Atomic frontend-to-controller diagnostic mailbox. */
} runtime;

static cm_result result(esp_err_t rc)
{ return rc == ESP_OK ? CM_OK : rc == ESP_ERR_INVALID_ARG ? CM_INVALID : CM_IO; }
static cm_result begin(void *ctx, const cm_config *config, uint8_t mode)
{
    (void)ctx;
    /* Only an explicit new preparation may recover a previous driver fault.
     * Worker stop has quiesced acquisition first; lifetime counters survive. */
    if (runtime.device->state == CMQ_FAULT && cmq_reset(runtime.device) != CMQ_OK)
        return CM_IO;
    const cm_acq_policy policies[] = {CM_ACQ_POLICY_SAFE, CM_ACQ_POLICY_MEDIUM, CM_ACQ_POLICY_HIGH};
    const cmq_axis axes[] = {CMQ_X, CMQ_Y, CMQ_Z};
    if (!cm_config_valid(config) || mode > CM_POLICY_HIGH) return CM_INVALID;
    esp_err_t rc = cm_acq_idf_configure(runtime.acquisition, policies[mode],
        axes[config->axis], config->range_g, CM_ACQ_MODE_SAFE);
    if (rc == ESP_OK) rc = cm_acq_idf_prepare(runtime.acquisition);
    return result(rc);
}
static cm_result ready(void *ctx, uint32_t *rate)
{
    (void)ctx;
    if (!cm_acq_idf_measurement_ready(runtime.acquisition)) return CM_BUSY;
    *rate = cm_acq_idf_measured_rate_millihz(runtime.acquisition);
    return *rate ? CM_OK : CM_IO;
}
static cm_result publish(void *ctx)
{ (void)ctx; return result(cm_acq_idf_publish(runtime.acquisition)); }
static bool published(void *ctx)
{ (void)ctx; return cm_acq_idf_publishing(runtime.acquisition); }
static cm_result stop(void *ctx)
{ (void)ctx; return result(cm_acq_idf_stop(runtime.acquisition)); }
static void controller(void *ctx)
{
    cm_session *s = ctx;
    uint32_t logged_generation = 0, logged_event = 0;
    TickType_t delay = pdMS_TO_TICKS(10);
    if (!delay) delay = 1;
    for (;;) {
        cm_session_worker_step(s, (uint64_t)esp_timer_get_time());
        if (s->worker_state == CMS_FAILED && logged_generation != s->worker_generation) {
            ESP_LOGE("contactmic_session", "source lifecycle failure generation=%" PRIu32,
                     s->worker_generation);
            logged_generation = s->worker_generation;
        }
        uint32_t event = __atomic_load_n(&runtime.boundary_event, __ATOMIC_SEQ_CST);
        if (event != logged_event) {
            ESP_LOGW("contactmic_session", "stream boundary reason=%u next_mode=%u"
                     " transition_tag=%" PRIu32 "; reconnect for a new session",
                     (unsigned)(event & 63u), (unsigned)((event >> 6) & 3u), event >> 8);
            logged_event = event;
        }
        vTaskDelay(delay);
    }
}
esp_err_t cm_session_idf_create(cmq_device *device, cm_source *source, cm_session **session)
{
    if (!device || !source || !session) return ESP_ERR_INVALID_ARG;
    *session = NULL;
    if (runtime.claimed) return ESP_ERR_INVALID_STATE;
    runtime.claimed = true; runtime.device = device;
    esp_err_t rc = cm_acq_idf_create(device, &runtime.acquisition);
    if (rc != ESP_OK) return rc;
    const cm_session_backend backend = {NULL, begin, ready, publish, published, stop};
    if (cm_session_init(&runtime.session, cm_acq_idf_core(runtime.acquisition),
                        &backend, CONFIG_CONTACTMIC_AUTO_CEILING) != CM_OK) return ESP_FAIL;
    if (xTaskCreate(controller, "contactmic_ctl", 4096, &runtime.session,
                    5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    *source = cm_session_source(&runtime.session); *session = &runtime.session;
    return ESP_OK;
}
bool cm_session_idf_observe(void *context, cm_transport *transport, uint32_t budget)
{
    cm_session *s = context;
    bool changed = cm_session_monitor(s, transport, (uint64_t)esp_timer_get_time(), budget);
    if (changed) {
        /* No logging or blocking work on the NimBLE owner. A single coalescing
         * atomic event carries diagnostics to the lower-priority controller. */
        uint32_t event = (uint32_t)(s->transitions & 0xffffffu) << 8;
        event |= ((uint32_t)transport->status.selected_mode << 6) | s->last_reason;
        __atomic_store_n(&runtime.boundary_event, event, __ATOMIC_SEQ_CST);
    }
    return changed;
}
