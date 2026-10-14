#include "contactmic_ble_idf.h"
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* API authority: esp-idf v5.5.5; esp-nimble submodule
 * 685675c0128deafdd201c9eb82e61d227364646c. All state below is host-task owned. */
static const char *TAG = "contactmic_ble";
#define CM_UUID(n) BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0, \
                                  0x93,0xf3,0xa3,0xb5,(n),0x00,0x40,0x7e)
static const ble_uuid128_t service_uuid = CM_UUID(1);
static const ble_uuid128_t audio_uuid = CM_UUID(2);
static const ble_uuid128_t control_uuid = CM_UUID(3);
static const ble_uuid128_t status_uuid = CM_UUID(4);
static uint16_t connection = BLE_HS_CONN_HANDLE_NONE;
static uint16_t audio_handle, control_handle, status_handle;
static uint8_t own_address;
static cm_transport transport;
static bool started, synced, status_subscribed, status_dirty;
static uint8_t response[CM_CONTROL_MAX_RESPONSE], response_read[CM_CONTROL_MAX_RESPONSE];
static size_t response_length, response_read_length;
static uint8_t status_read[CM_STATUS_SIZE];
static bool status_read_valid;
static struct ble_npl_callout pump_timer;
static uint32_t last_status_tick;
static uint32_t interval_ticks(uint32_t ms)
{ uint32_t n = ble_npl_time_ms_to_ticks32(ms); return n ? n : 1; }
static int gap_event(struct ble_gap_event *, void *);

static int access_characteristic(uint16_t conn, uint16_t handle,
                                  struct ble_gatt_access_ctxt *ctx, void *arg)
{
    (void)arg;
    if (conn != connection || conn == BLE_HS_CONN_HANDLE_NONE) return BLE_ATT_ERR_UNLIKELY;
    if (ctx->op == BLE_GATT_ACCESS_OP_WRITE_CHR && handle == control_handle) {
        uint16_t n = OS_MBUF_PKTLEN(ctx->om);
        if (n < CM_CONTROL_HEADER_SIZE || n > CM_CONTROL_MAX_REQUEST)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t request[CM_CONTROL_MAX_REQUEST];
        if (ble_hs_mbuf_to_flat(ctx->om, request, sizeof(request), &n)) return BLE_ATT_ERR_UNLIKELY;
        cm_result rc = cm_transport_request(&transport, request, n,
                                             response, sizeof(response), &response_length);
        if (rc == CM_UNSUPPORTED) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
        if (rc != CM_OK) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        status_dirty = true;
        return 0; /* Application result is in the versioned read response. */
    }
    if (ctx->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    if (handle == control_handle) {
        if (!ctx->offset) {
            memcpy(response_read, response, response_length);
            response_read_length = response_length;
        }
        if (!response_read_length) return BLE_ATT_ERR_READ_NOT_PERMITTED;
        /* NimBLE applies the requested offset; retain one immutable value across
         * the complete ATT long-read. Never independently slice it here. */
        return os_mbuf_append(ctx->om, response_read, response_read_length) == 0 ?
               0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (handle == status_handle) {
        if (!ctx->offset) {
            cm_transport_snapshot(&transport); size_t n = 0;
            if (cm_status_encode(&transport.status, 0, status_read, sizeof(status_read), &n) != CM_OK)
                return BLE_ATT_ERR_UNLIKELY;
            status_read_valid = true;
        }
        if (!status_read_valid) return BLE_ATT_ERR_UNLIKELY;
        return os_mbuf_append(ctx->om, status_read, sizeof(status_read)) == 0 ?
               0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &service_uuid.u,
     .characteristics = (struct ble_gatt_chr_def[]) {
        {.uuid=&audio_uuid.u, .access_cb=access_characteristic,
         .flags=BLE_GATT_CHR_F_NOTIFY, .val_handle=&audio_handle},
        {.uuid=&control_uuid.u, .access_cb=access_characteristic,
         .flags=BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE, .val_handle=&control_handle},
        {.uuid=&status_uuid.u, .access_cb=access_characteristic,
         .flags=BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle=&status_handle},
        {0}
     }},
    {0}
};
static void advertise(void)
{
    if (!synced || connection != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) return;
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128 = 1; fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) { ESP_LOGW(TAG, "advertising fields: %d", rc); return; }
    const char *name = ble_svc_gap_device_name();
    fields = (struct ble_hs_adv_fields){0};
    fields.name = (uint8_t *)name; fields.name_len = strlen(name); fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&fields); /* Name and 128-bit UUID do not fit one legacy AD. */
    if (rc) { ESP_LOGW(TAG, "scan response: %d", rc); return; }
    struct ble_gap_adv_params params = {.conn_mode=BLE_GAP_CONN_MODE_UND,
                                        .disc_mode=BLE_GAP_DISC_MODE_GEN};
    rc = ble_gap_adv_start(own_address, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc) ESP_LOGW(TAG, "advertising start: %d (bounded retry follows)", rc);
}
static bool notify_audio(void *ctx, const uint8_t *data, size_t n)
{
    (void)ctx;
    if (connection == BLE_HS_CONN_HANDLE_NONE || n > (size_t)ble_att_mtu(connection) - 3)
        return false;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, n);
    if (!om) return false;
    /* NimBLE consumes om regardless of the result; do not free it a second time. */
    return ble_gatts_notify_custom(connection, audio_handle, om) == 0;
}
static void notify_status(void)
{
    if (!status_subscribed || connection == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t bytes[CM_STATUS_EVENT_SIZE]; size_t n = 0;
    if (cm_status_encode(&transport.status, 1, bytes, sizeof(bytes), &n) != CM_OK) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(bytes, n);
    if (om && ble_gatts_notify_custom(connection, status_handle, om) == 0) status_dirty = false;
}
static void pump(struct ble_npl_event *event)
{
    (void)event;
    uint32_t now = ble_npl_time_get();
    bool periodic = now - last_status_tick >= interval_ticks(1000);
    if (periodic) last_status_tick = now;
    if (connection != BLE_HS_CONN_HANDLE_NONE) {
        /* Each invocation is bounded, including under a slow or absent consumer. */
        for (unsigned i=0; i<4; ++i)
            if (cm_transport_pump(&transport, notify_audio, NULL) != CM_OK) break;
        if (periodic) {
            uint8_t tx=0, rx=0;
            if (ble_gap_read_le_phy(connection, &tx, &rx) == 0) transport.status.phy = tx;
            cm_transport_snapshot(&transport); status_dirty = true;
        }
        if (status_dirty) notify_status();
    } else if (periodic) advertise();
    ble_npl_callout_reset(&pump_timer, interval_ticks(5));
}
static void clear_connection(void)
{
    cm_transport_disconnect(&transport); connection = BLE_HS_CONN_HANDLE_NONE;
    status_subscribed = false; status_read_valid = false;
    response_length = response_read_length = 0;
}
static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) { advertise(); break; }
        connection = event->connect.conn_handle;
        cm_transport_connect(&transport); status_dirty = true;
        /* Requests, not claims of negotiated transport capacity. */
        int rc = ble_gap_set_data_len(connection, 251, 2120);
        if (rc) ESP_LOGI(TAG, "data-length request not accepted: %d", rc);
        rc = ble_gap_set_prefered_le_phy(connection, BLE_GAP_LE_PHY_2M_MASK,
                                        BLE_GAP_LE_PHY_2M_MASK, 0);
        if (rc) ESP_LOGI(TAG, "2M PHY request not accepted: %d", rc);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        clear_connection(); advertise(); break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise(); break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle != connection) break;
        if (event->subscribe.attr_handle == audio_handle)
            cm_transport_subscribe(&transport, event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == status_handle) {
            status_subscribed = event->subscribe.cur_notify; status_dirty = true;
        }
        break;
    case BLE_GAP_EVENT_MTU:
        if (event->mtu.conn_handle == connection) {
            cm_transport_mtu(&transport, ble_att_mtu(connection)); status_dirty = true;
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.conn_handle == connection && event->notify_tx.attr_handle == audio_handle &&
            !event->notify_tx.indication && event->notify_tx.status) {
            cm_transport_notify_failed(&transport); status_dirty = true;
        }
        break;
    default: break;
    }
    return 0;
}
static void on_reset(int reason)
{
    synced = false; clear_connection(); ESP_LOGW(TAG, "NimBLE reset: %d", reason);
}
static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) || ble_hs_id_infer_auto(0, &own_address)) {
        ESP_LOGE(TAG, "Cannot establish BLE identity"); return;
    }
    synced = true; advertise();
}
static void host_task(void *arg)
{
    (void)arg; nimble_port_run(); nimble_port_freertos_deinit();
}
esp_err_t cm_ble_idf_start(const cm_source *source)
{
    if (started) return ESP_ERR_INVALID_STATE;
    /* Random boot prefix reduces cross-boot identity collisions; connection
     * generation, not this randomness, is the stale-callback exclusion mechanism. */
    if (cm_transport_init(&transport, source, esp_random() & 0x7fffffffu) != CM_OK)
        return ESP_ERR_INVALID_ARG;
    esp_err_t rc = nvs_flash_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed; preserve data and resolve before retry: %s", esp_err_to_name(rc));
        return rc;
    }
    rc = nimble_port_init(); if (rc != ESP_OK) return rc;
    ble_hs_cfg.reset_cb = on_reset; ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init(); ble_svc_gatt_init();
    int host_rc = ble_svc_gap_device_name_set("S3-ContactMic");
    if (!host_rc) host_rc = ble_att_set_preferred_mtu(247);
    if (!host_rc) host_rc = ble_gatts_count_cfg(services);
    if (!host_rc) host_rc = ble_gatts_add_svcs(services);
    if (host_rc) { (void)nimble_port_deinit(); return ESP_FAIL; }
    ble_npl_callout_init(&pump_timer, nimble_port_get_dflt_eventq(), pump, NULL);
    ble_npl_callout_reset(&pump_timer, interval_ticks(5));
    started = true; nimble_port_freertos_init(host_task);
    return ESP_OK;
}
