#ifndef CM_USB_IDF_H
#define CM_USB_IDF_H
#include "contactmic_board.h"
#include "esp_err.h"
/* Device/context lives for this boot. source_ready=false enumerates diagnostics
 * and counted failure silence; it never substitutes a synthetic microphone. */
esp_err_t cmu_idf_start(cmq_device *device, bool source_ready);
#endif
