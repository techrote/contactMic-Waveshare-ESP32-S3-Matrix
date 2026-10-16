#include "cm_usb_idf.h"
#include "cm_usb.h"
#include "cm_usb_descriptors.h"
#include "contactmic_acquisition_idf.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "device/dcd.h"
#include "sdkconfig.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#ifndef CMU_SOURCE_REVISION
#define CMU_SOURCE_REVISION "unknown"
#endif

static struct {
    cmu_core core;
    cm_acq_idf *acquisition;
    cmq_device *device;
    usb_phy_handle_t phy;
    bool claimed, source_ready, stream_open, reconnect_pending, mounted;
    uint32_t bus_epoch, observed_epoch, boot_id, backend_failure, drdy_levels_seen;
    uint64_t reconnect_at;
    char serial[13], product[24], elf_hash[65];
    char rx[CMU_CONTROL_BYTES], tx[3072];
    size_t rx_used, tx_used, tx_sent;
    bool rx_overflow;
} runtime;

static uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }
static void increment(uint64_t *p) { if (*p != UINT64_MAX) ++*p; }
static cmu_result result(esp_err_t r) { return r == ESP_OK ? CMU_OK : CMU_IO; }
/* Controller writes one atomic diagnostic word; USB never reads live driver
 * internals or performs I2C to explain a failed preparation. Retain last failure. */
static cmu_result backend_result(esp_err_t rc, uint32_t stage)
{
    if (rc != ESP_OK)
        __atomic_store_n(&runtime.backend_failure,
            (stage << 16) | ((uint32_t)rc & 0xffffu), __ATOMIC_RELEASE);
    return result(rc);
}
static cmu_result source_begin(void *p, const cmu_config *config, uint32_t mode)
{
    (void)p;
    if (!runtime.source_ready || !runtime.acquisition || mode > CMU_HIGH) return CMU_IO;
    __atomic_store_n(&runtime.drdy_levels_seen, 0u, __ATOMIC_RELEASE);
    if (runtime.device->state == CMQ_FAULT && cmq_reset(runtime.device) != CMQ_OK)
        return backend_result(ESP_FAIL, 1);
    const cm_acq_policy policies[] = {CM_ACQ_POLICY_SAFE, CM_ACQ_POLICY_MEDIUM, CM_ACQ_POLICY_HIGH};
    const cmq_axis axes[] = {CMQ_X, CMQ_Y, CMQ_Z};
    if (!cmu_config_valid(config)) return CMU_INVALID;
    esp_err_t rc = cm_acq_idf_configure(runtime.acquisition, policies[mode],
        axes[config->axis], (uint8_t)config->range_g, CM_ACQ_MODE_SAFE);
    if (rc != ESP_OK) return backend_result(rc, 2);
    return backend_result(cm_acq_idf_prepare(runtime.acquisition), 3);
}
static cmu_result source_ready(void *p, uint32_t *rate)
{
    (void)p;
    if (!runtime.acquisition) return CMU_IO;
    /* Slow controller observations distinguish a static pin from an ISR/count
     * problem. This is NOT a sampled waveform or source-rate measurement. */
    uint32_t level = gpio_get_level((gpio_num_t)CMQ_INT2_GPIO) ? 2u : 1u;
    (void)__atomic_fetch_or(&runtime.drdy_levels_seen, level, __ATOMIC_RELEASE);
    if (!cm_acq_idf_measurement_ready(runtime.acquisition)) return CMU_BUSY;
    *rate = cm_acq_idf_measured_rate_millihz(runtime.acquisition);
    return *rate ? CMU_OK : backend_result(ESP_FAIL, 4);
}
static cmu_result source_publish(void *p)
{ (void)p; return runtime.acquisition ? backend_result(cm_acq_idf_publish(runtime.acquisition), 6) : CMU_IO; }
static bool source_published(void *p)
{ (void)p; return runtime.acquisition && cm_acq_idf_publishing(runtime.acquisition); }
static cmu_result source_stop(void *p)
{ (void)p; return runtime.acquisition ? backend_result(cm_acq_idf_stop(runtime.acquisition), 5) : CMU_OK; }
static bool source_peek(void *p, size_t n, cmu_sample *out)
{
    (void)p;
    if (!runtime.acquisition || !out) return false;
    cm_acq_sample sample;
    if (!cm_acq_peek(cm_acq_idf_core(runtime.acquisition), n, &sample)) return false;
    *out = (cmu_sample){sample.index, sample.value}; return true;
}
static size_t source_consume(void *p, size_t n)
{ (void)p; return runtime.acquisition ? cm_acq_consume(cm_acq_idf_core(runtime.acquisition), n) : 0; }
static size_t source_count(void *p)
{ (void)p; return runtime.acquisition ? cm_acq_count(cm_acq_idf_core(runtime.acquisition)) : 0; }
static bool source_snapshot(void *p, cmu_source_counts *out)
{
    (void)p;
    cm_acq_counters counts;
    if (!runtime.acquisition || !cm_acq_try_snapshot(cm_acq_idf_core(runtime.acquisition), &counts)) return false;
    *out = (cmu_source_counts){counts.drdy_edges, counts.successful_reads,
        counts.inferred_missed_intervals, counts.i2c_failures, counts.ring_overruns, counts.source_faults};
    return true;
}
static void controller(void *p)
{
    (void)p;
    for (;;) { cmu_worker_step(&runtime.core, now_us()); vTaskDelay(pdMS_TO_TICKS(10)); }
}
static void gate(cmu_reason reason)
{
    runtime.stream_open = false;
    (void)cmu_request_stream(&runtime.core, false, reason, now_us());
}
static void synchronize_bus_epoch(void)
{
    uint32_t epoch = __atomic_load_n(&runtime.bus_epoch, __ATOMIC_SEQ_CST);
    if (epoch != runtime.observed_epoch) {
        runtime.observed_epoch = epoch;
        runtime.mounted = false;
        gate(CMU_REASON_RESET);
        runtime.rx_used = runtime.tx_used = runtime.tx_sent = 0;
        runtime.rx_overflow = false;
    }
}
void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
    (void)rhport; (void)in_isr;
    /* ISR only records invalidation. No I2C, DSP, logging, or lifecycle mutex. */
    if (eventid == DCD_EVENT_BUS_RESET || eventid == DCD_EVENT_UNPLUGGED)
        (void)__atomic_add_fetch(&runtime.bus_epoch, 1u, __ATOMIC_SEQ_CST);
}
uint8_t const *tud_descriptor_device_cb(void) { return cmu_device_descriptor; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{ return index == 0 ? cmu_configuration_descriptor : NULL; }
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t language)
{
    static uint16_t text[32];
    if (index == 0) { text[0] = 0x0304; text[1] = 0x0409; return text; }
    if (language != 0x0409) return NULL;
    const char *strings[] = {NULL, "techrote development", runtime.product, runtime.serial, "CM USB control"};
    if (index >= sizeof(strings)/sizeof(strings[0])) return NULL;
    size_t n = strlen(strings[index]); if (n > 31) n = 31;
    for (size_t i=0; i<n; ++i) text[1+i] = (uint8_t)strings[index][i];
    text[0] = (uint16_t)(0x0300u | (2u*n+2u)); return text;
}
void tud_mount_cb(void) { synchronize_bus_epoch(); runtime.mounted = true; }
void tud_umount_cb(void) { runtime.mounted = false; gate(CMU_REASON_DETACH); }
void tud_suspend_cb(bool remote_wakeup)
{
    (void)remote_wakeup;
    runtime.mounted = false;
    gate(CMU_REASON_SUSPEND);
    (void)tud_audio_clear_ep_in_ff();
    /* Conservative lifecycle contract: suspend disconnects the function rather
     * than replaying an already-queued pre-suspend transfer. Re-enumeration then
     * bus-reset clears hardware/class FIFOs. Recording must be reopened by host. */
    (void)tud_disconnect();
    runtime.reconnect_pending = true; runtime.reconnect_at = now_us() + 250000u;
}
void tud_resume_cb(void) { /* No old generation is resumed in-place. */ }
static bool start_endpoint(void)
{
    synchronize_bus_epoch();
    if (!runtime.mounted) return false;
    if (!runtime.stream_open) {
        runtime.stream_open = true;
        (void)cmu_request_stream(&runtime.core, true, CMU_REASON_NONE, now_us());
    }
    return true;
}
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *r)
{
    (void)rhport;
    if (!cmu_uac_alt_valid(r->bmRequestType, r->bRequest, r->wValue, r->wIndex, r->wLength)) return false;
    if (r->wValue == 1) return start_endpoint();
    else if (runtime.stream_open) gate(CMU_REASON_STOP);
    return true;
}
bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *r)
{
    (void)rhport;
    if (r->wIndex != CMU_AUDIO_ITF) return false;
    gate(CMU_REASON_STOP); return true;
}
bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t func_id, uint8_t ep, uint8_t alt)
{
    (void)rhport;
    /* TinyUSB 0.18.0 passes the audio-function index here, not bInterfaceNumber. */
    if (func_id != 0 || ep != CMU_AUDIO_EP || alt != 1) return false;
    /* TinyUSB invokes this also for the first transfer, before set_itf_cb. */
    if (!start_endpoint()) return false;
    uint8_t bytes[CMU_FRAME_BYTES];
    cmu_render(&runtime.core, bytes, now_us());
    cmu_usb_written(&runtime.core, tud_audio_write(bytes, sizeof(bytes)));
    return true;
}
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *r)
{
    uint8_t data[14]; uint16_t length;
    if (!cmu_uac_get(r->bmRequestType, r->bRequest, r->wValue, r->wIndex, r->wLength, data, &length)) return false;
    return tud_audio_buffer_and_schedule_control_xfer(rhport, r, data, length);
}
/* All unadvertised class controls, including SET_CUR on the fixed clock, stall
 * via the pinned stack's weak default callbacks. */

static void format_reply(uint32_t id, cmu_result r)
{
    if (runtime.tx_sent != runtime.tx_used) { increment(&runtime.core.counters.cdc_dropped_replies); return; }
    cmu_core *c = &runtime.core;
    cmu_snapshot_source(c);
    cmu_counters *d = &c->counters;
    cmu_source_counts *s = &c->source;
    const cmq_device *q = runtime.device;
    char qmi[100];
    if (q && q->who_am_i == 5)
        (void)snprintf(qmi, sizeof(qmi), "{\"address\":%u,\"who\":%u,\"revision\":%u}", q->address, q->who_am_i, q->revision);
    else (void)snprintf(qmi, sizeof(qmi), "null");
    int n = snprintf(runtime.tx, sizeof(runtime.tx),
        "{\"schema\":\"cm009.v1\",\"request_id\":%" PRIu32 ",\"result\":%u,"
        "\"serial\":\"%s\",\"boot_id\":%" PRIu32 ",\"reset_reason\":%u,"
        "\"firmware\":\"%s\",\"elf_sha256\":\"%s\",\"source_available\":%s,\"qmi\":%s,"
        "\"generation\":%" PRIu32 ",\"state\":%" PRIu32 ",\"reason\":%" PRIu32 ",\"backend_failure\":%" PRIu32 ",\"drdy_levels_seen\":%" PRIu32 ","
        "\"source_rate_millihz\":%" PRIu32 ",\"mode\":%" PRIu32 ",\"uac_rate\":48000,"
        "\"axis\":%" PRIu32 ",\"range_g\":%" PRIu32 ",\"policy\":%" PRIu32 ","
        "\"gain_milli\":%" PRIu32 ",\"cutoff_millihz\":%" PRIu32 ",\"led_policy\":\"untouched\","
        "\"source_snapshot_fresh\":%s,\"uptime_us\":%" PRIu64 ",\"output_position\":%" PRIu64 ","
        "\"queue\":%u,\"queue_high\":%" PRIu32 ",\"queue_low\":%" PRIu32 ",\"correction_ppm\":%" PRId32 ","
        "\"source\":{\"intervals\":%" PRIu64 ",\"reads\":%" PRIu64 ",\"missed\":%" PRIu64 ","
        "\"i2c\":%" PRIu64 ",\"overruns\":%" PRIu64 ",\"faults\":%" PRIu64 "},"
        "\"usb\":{\"output_samples\":%" PRIu64 ",\"startup_silence\":%" PRIu64 ","
        "\"starvation_silence\":%" PRIu64 ",\"failed_silence\":%" PRIu64 ","
        "\"source_gaps\":%" PRIu64 ",\"gap_zeros\":%" PRIu64 ",\"duplicate_source\":%" PRIu64 ","
        "\"large_gap_resets\":%" PRIu64 ",\"dsp_clips\":%" PRIu64 ",\"raw_clips\":%" PRIu64 ","
        "\"resampler_resets\":%" PRIu64 ",\"queue_discards\":%" PRIu64 ","
        "\"clock_updates\":%" PRIu64 ",\"clock_saturated\":%" PRIu64 ","
        "\"short_writes\":%" PRIu64 ",\"late_frames\":%" PRIu64 ",\"transitions\":%" PRIu64 ","
        "\"rejected_controls\":%" PRIu64 ",\"cdc_overflows\":%" PRIu64 ",\"cdc_dropped_replies\":%" PRIu64 "}}\n",
        id, (unsigned)r, runtime.serial, runtime.boot_id, (unsigned)esp_reset_reason(),
        CMU_SOURCE_REVISION, runtime.elf_hash, runtime.source_ready ? "true" : "false", qmi,
        c->generation, c->state, c->reason, __atomic_load_n(&runtime.backend_failure, __ATOMIC_ACQUIRE),
        __atomic_load_n(&runtime.drdy_levels_seen, __ATOMIC_ACQUIRE), c->rate, c->mode,
        c->config.axis, c->config.range_g, c->config.policy, c->config.gain_milli, c->config.cutoff_millihz,
        c->source_snapshot_fresh ? "true" : "false", now_us(), c->generation_output_samples, (unsigned)source_count(NULL), c->queue_high,
        c->queue_low == UINT32_MAX ? 0 : c->queue_low, c->correction_ppm,
        s->intervals, s->reads, s->missed, s->i2c, s->overruns, s->faults,
        d->output_samples, d->startup_silence, d->starvation_silence, d->failed_silence,
        d->source_gaps, d->gap_zeros, d->duplicate_source, d->large_gap_resets,
        d->dsp_clips, d->raw_clips, d->resampler_resets, d->queue_discards,
        d->clock_updates, d->clock_saturated, d->usb_short_writes, d->usb_late_frames,
        d->transitions, d->rejected_controls, d->cdc_overflows, d->cdc_dropped_replies);
    if (n < 0 || (size_t)n >= sizeof(runtime.tx)) {
        runtime.tx_used = runtime.tx_sent = 0; increment(&d->cdc_dropped_replies); return;
    }
    runtime.tx_used = (size_t)n; runtime.tx_sent = 0;
}
static void cdc_pump(void)
{
    synchronize_bus_epoch();
    if (!tud_cdc_connected()) {
        runtime.rx_used = runtime.tx_used = runtime.tx_sent = 0;
        runtime.rx_overflow = false; return;
    }
    uint8_t bytes[64];
    uint32_t n = tud_cdc_read(bytes, sizeof(bytes));
    for (uint32_t i=0; i<n; ++i) {
        uint8_t ch = bytes[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            uint32_t id = 0;
            cmu_result r = runtime.rx_overflow ? CMU_INVALID :
                cmu_command_line(&runtime.core, runtime.rx, runtime.rx_used, &id);
            format_reply(id, r); runtime.rx_used = 0; runtime.rx_overflow = false;
        } else if (runtime.rx_used < sizeof(runtime.rx) - 1u && !runtime.rx_overflow) {
            runtime.rx[runtime.rx_used++] = (char)ch;
        } else if (!runtime.rx_overflow) {
            runtime.rx_overflow = true; increment(&runtime.core.counters.cdc_overflows);
        }
    }
    if (runtime.tx_sent < runtime.tx_used) {
        size_t nbytes = runtime.tx_used - runtime.tx_sent;
        uint32_t room = tud_cdc_write_available();
        if (nbytes > 64u) nbytes = 64u;
        if (nbytes > room) nbytes = room;
        runtime.tx_sent += tud_cdc_write(runtime.tx + runtime.tx_sent, (uint32_t)nbytes);
        (void)tud_cdc_write_flush();
    }
}
/* Event-driven CDC service remains live even when tud_task_ext continuously
 * receives audio events and its outer idle loop does not get a timeout. */
void tud_cdc_rx_cb(uint8_t instance) { if (instance == 0) cdc_pump(); }
void tud_cdc_tx_complete_cb(uint8_t instance) { if (instance == 0) cdc_pump(); }
void tud_cdc_line_state_cb(uint8_t instance, bool dtr, bool rts)
{
    (void)rts;
    if (instance != 0 || dtr) return;
    runtime.rx_used = runtime.tx_used = runtime.tx_sent = 0;
    runtime.rx_overflow = false;
    tud_cdc_read_flush(); (void)tud_cdc_write_clear();
    /* Closing diagnostics does not close the independent audio capture. */
}
static void usb_task(void *p)
{
    (void)p;
    for (;;) {
        tud_task_ext(1, false);
        synchronize_bus_epoch();
        if (runtime.mounted && !tud_mounted()) { runtime.mounted = false; gate(CMU_REASON_RESET); }
        cmu_tick(&runtime.core, now_us()); cdc_pump();
        if (runtime.reconnect_pending && now_us() >= runtime.reconnect_at) {
            runtime.reconnect_pending = false; (void)tud_connect();
        }
    }
}
esp_err_t cmu_idf_start(cmq_device *device, bool usable)
{
    if (runtime.claimed) return ESP_ERR_INVALID_STATE;
    runtime.claimed = true; runtime.device = device; runtime.source_ready = usable;
    uint8_t mac[6];
    esp_err_t rc = esp_efuse_mac_get_default(mac);
    if (rc != ESP_OK) return rc;
    (void)snprintf(runtime.serial, sizeof(runtime.serial), "%02X%02X%02X%02X%02X%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    (void)snprintf(runtime.product, sizeof(runtime.product), "CM USB %s", runtime.serial);
    /* The string helper truncates to CONFIG_APP_RETRIEVE_LEN_ELF_SHA (default 9).
     * Build provenance requires all 32 bytes from the linked image descriptor. */
    const esp_app_desc_t *app = esp_app_get_description();
    for (size_t i = 0; i < sizeof(app->app_elf_sha256); ++i)
        (void)snprintf(runtime.elf_hash + 2u*i, 3, "%02x", app->app_elf_sha256[i]);
    runtime.boot_id = esp_random();
    if (usable && cm_acq_idf_create(device, &runtime.acquisition) != ESP_OK) runtime.source_ready = false;
    const cmu_backend backend = {NULL, source_begin, source_ready, source_publish,
        source_published, source_stop, source_peek, source_consume, source_count, source_snapshot};
    if (cmu_init(&runtime.core, &backend) != CMU_OK) return ESP_FAIL;
    runtime.core.auto_ceiling = CONFIG_CONTACTMIC_USB_AUTO_CEILING;
    if (xTaskCreate(controller, "cm_usb_source", 4096, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    const usb_phy_config_t phy = {.controller=USB_PHY_CTRL_OTG, .target=USB_PHY_TARGET_INT,
        .otg_mode=USB_OTG_MODE_DEVICE, .otg_speed=USB_PHY_SPEED_FULL};
    rc = usb_new_phy(&phy, &runtime.phy); if (rc != ESP_OK) return rc;
    const tusb_rhport_init_t usb = {.role=TUSB_ROLE_DEVICE, .speed=TUSB_SPEED_FULL};
    if (!tusb_init(0, &usb)) return ESP_FAIL;
    if (xTaskCreate(usb_task, "cm_usb", 6144, NULL, 8, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
