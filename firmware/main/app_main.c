#include "contactmic_ble_idf.h"
#include "contactmic_board_idf.h"
#include "contactmic_session_idf.h"
#include "esp_log.h"

void app_main(void)
{
    static cmq_idf_bus bus;
    static cmq_device device;
    cmq_bus interface;
    cm_source source;
    cm_session *session = NULL;
    esp_err_t rc = cmq_idf_open(&bus, CMQ_I2C_MAX_HZ, &interface);
    if (rc == ESP_OK) {
        cmq_result q = cmq_init(&device, &interface, CMQ_I2C_MAX_HZ);
        if (q == CMQ_OK) q = cmq_probe(&device);
        if (q == CMQ_OK) q = cmq_reset(&device);
        if (q != CMQ_OK) {
            ESP_LOGE("contactmic", "QMI initialization failed (%d); source unavailable", (int)q);
            rc = ESP_FAIL;
        }
    }
    if (rc == ESP_OK) {
        ESP_LOGI("contactmic", "QMI addr=0x%02x WHO=0x%02x revision=0x%02x; LEDs untouched",
                 device.address, device.who_am_i, device.revision);
        /* One structured identity per boot lets CM-007 correlate serial evidence
         * with the physical sensor without changing protocol v1. The LED field is
         * policy, not an observation; the operator still records the visible state. */
        ESP_LOGI("contactmic_hw",
                 "CM_HW {\"schema\":1,\"qmi_address\":%u,\"who_am_i\":%u,"
                 "\"revision\":%u,\"i2c_hz\":%u,\"led_policy\":\"untouched\"}",
                 (unsigned)device.address, (unsigned)device.who_am_i,
                 (unsigned)device.revision, (unsigned)CMQ_I2C_MAX_HZ);
        rc = cm_session_idf_create(&device, &source, &session);
    }
    if (rc == ESP_OK) {
        ESP_LOGI("contactmic", "CM-006 integrated source; no physical performance qualification");
        rc = cm_ble_idf_start_observed(&source, cm_session_idf_observe, session);
    } else {
        ESP_LOGE("contactmic", "source initialization failed: %s; advertising diagnostics only",
                 esp_err_to_name(rc));
        rc = cm_ble_idf_start(NULL);
    }
    if (rc != ESP_OK) ESP_LOGE("contactmic", "BLE initialization failed: %s", esp_err_to_name(rc));
}
