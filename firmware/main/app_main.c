#include "contactmic_ble_idf.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_LOGI("contactmic", "CM-004 custom GATT service; real acquisition is not integrated yet.");
    /* NULL is honest source-unavailable behavior, not synthetic microphone audio.
     * CM-006 will supply the accepted acquisition adapter. No QMI or LEDs touched. */
    esp_err_t rc = cm_ble_idf_start(NULL);
    if (rc != ESP_OK) ESP_LOGE("contactmic", "BLE initialization failed: %s", esp_err_to_name(rc));
}
