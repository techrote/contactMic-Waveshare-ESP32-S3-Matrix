#ifndef CONTACTMIC_SESSION_IDF_H
#define CONTACTMIC_SESSION_IDF_H
#include "contactmic_session.h"
#include "contactmic_board.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Create once before NimBLE starts. Device must be probed/reset and remain alive
 * for this boot. No BLE callback performs these initialization operations. */
esp_err_t cm_session_idf_create(cmq_device *, cm_source *source_out, cm_session **session_out);
/* Serialized NimBLE-owner observer; performs no sensor I/O and never waits. */
bool cm_session_idf_observe(void *, cm_transport *, uint32_t notification_budget_per_second);
#ifdef __cplusplus
}
#endif
#endif
