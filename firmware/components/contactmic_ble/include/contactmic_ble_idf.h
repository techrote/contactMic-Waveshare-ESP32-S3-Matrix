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
/* Optional host-owner observer, called before pumping and after connect/MTU.
 * It may update policy/status, but must not perform sensor I/O or block. */
typedef bool (*cm_ble_observer_fn)(void *, cm_transport *, uint32_t notification_budget_per_second);
esp_err_t cm_ble_idf_start_observed(const cm_source *, cm_ble_observer_fn, void *context);
#ifdef __cplusplus
}
#endif
#endif
