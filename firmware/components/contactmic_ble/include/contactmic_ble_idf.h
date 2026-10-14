#ifndef CONTACTMIC_BLE_IDF_H
#define CONTACTMIC_BLE_IDF_H
#include "contactmic_ble.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Start once, from app_main before publishing any source samples. The interface
 * is copied; its context must live for the boot lifetime. NULL advertises an
 * honest source-unavailable service. This function does not start acquisition. */
esp_err_t cm_ble_idf_start(const cm_source *source);
#ifdef __cplusplus
}
#endif
#endif
