#include "contactmic_board_idf.h"
#include "cm_usb_idf.h"
#include "esp_log.h"

void app_main(void)
{
    static cmq_idf_bus bus;
    static cmq_device device;
    cmq_bus interface;
    bool ready = false;
    if (cmq_idf_open(&bus, CMQ_I2C_MAX_HZ, &interface) == ESP_OK &&
        cmq_init(&device, &interface, CMQ_I2C_MAX_HZ) == CMQ_OK &&
        cmq_probe(&device) == CMQ_OK && cmq_reset(&device) == CMQ_OK) ready = true;
    /* No Bluetooth/Wi-Fi initialization. Same-cable CDC reports a failed sensor
     * even when UAC enumeration succeeds. No synthetic sensor signal fallback. */
    esp_err_t rc = cmu_idf_start(&device, ready);
    if (rc != ESP_OK) ESP_LOGE("cm_usb", "USB initialization failed: %s", esp_err_to_name(rc));
}
