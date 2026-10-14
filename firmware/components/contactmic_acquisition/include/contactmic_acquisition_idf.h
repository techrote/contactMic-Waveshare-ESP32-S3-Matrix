#ifndef CONTACTMIC_ACQUISITION_IDF_H
#define CONTACTMIC_ACQUISITION_IDF_H

#include "contactmic_acquisition.h"
#include "contactmic_board.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cm_acq_idf cm_acq_idf;

/*
 * The adapter owns GPIO13 interrupt handling and the high-priority acquisition
 * task. The supplied cmq_device remains externally allocated and must outlive it.
 * Probe/reset remain board-initialization responsibilities; this adapter owns
 * configure/start/read/stop after creation.
 */
esp_err_t cm_acq_idf_create(cmq_device *device, cm_acq_idf **out);
void cm_acq_idf_destroy(cm_acq_idf *ctx);

esp_err_t cm_acq_idf_configure(cm_acq_idf *ctx, cm_acq_policy policy,
                               cmq_axis axis, uint8_t range_g,
                               cm_acq_mode auto_ceiling);
esp_err_t cm_acq_idf_start(cm_acq_idf *ctx);
esp_err_t cm_acq_idf_stop(cm_acq_idf *ctx);
/* Integration controller only. prepare performs slow configure/start settling
 * outside BLE and drains calibration samples. publish is acknowledged by the
 * acquisition task only after rebasing the index and clearing the ring. */
esp_err_t cm_acq_idf_prepare(cm_acq_idf *ctx);
esp_err_t cm_acq_idf_publish(cm_acq_idf *ctx);
bool cm_acq_idf_publishing(const cm_acq_idf *ctx);

/* AUTO only consumes explicit evidence supplied by later integration policy.
 * A lower transport ceiling clamps immediately; degrade steps down one mode.
 * No threshold is inferred from simulator timing here. */
esp_err_t cm_acq_idf_transition(cm_acq_idf *ctx, bool degrade,
                                cm_acq_mode transport_ceiling);

bool cm_acq_idf_measurement_ready(const cm_acq_idf *ctx);
uint32_t cm_acq_idf_measured_rate_millihz(const cm_acq_idf *ctx);
cm_acq_mode cm_acq_idf_current_mode(const cm_acq_idf *ctx);
cm_acq *cm_acq_idf_core(cm_acq_idf *ctx);

#ifdef __cplusplus
}
#endif
#endif
