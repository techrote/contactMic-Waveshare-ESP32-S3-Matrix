#ifndef CONTACTMIC_BOARD_IDF_H
#define CONTACTMIC_BOARD_IDF_H
#include "contactmic_board.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Initialize storage with {0}. Keep alive for every cmq_device using its interface.
 * Close only after the driver/sensor is stopped and the owning task is quiescent. */
typedef struct {
    i2c_master_bus_handle_t handle;
    i2c_master_dev_handle_t devices[2];
} cmq_idf_bus;
esp_err_t cmq_idf_open(cmq_idf_bus *, uint32_t clock_hz, cmq_bus *interface_out);
esp_err_t cmq_idf_close(cmq_idf_bus *);
#ifdef __cplusplus
}
#endif
#endif
