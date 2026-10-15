#ifndef CM_USB_DESCRIPTORS_H
#define CM_USB_DESCRIPTORS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CMU_AUDIO_DESC_LEN 118u
#define CMU_CONFIG_DESC_LEN (9u + CMU_AUDIO_DESC_LEN + 66u)
#define CMU_AUDIO_ITF 1u
#define CMU_AUDIO_EP 0x81u
#define CMU_CLOCK_ENTITY 4u
#ifndef CMU_USB_VID
#define CMU_USB_VID 0xCAFEu /* TinyUSB demonstration identity; NOT a production allocation */
#endif
#ifndef CMU_USB_PID
#define CMU_USB_PID 0x4014u
#endif
extern const uint8_t cmu_device_descriptor[18];
extern const uint8_t cmu_configuration_descriptor[CMU_CONFIG_DESC_LEN];
/* UAC2 GET_CUR / GET_RANGE fixed clock. Pure bytes used by actual EP0 callback. */
bool cmu_uac_get(uint8_t request_type, uint8_t request, uint16_t value,
                 uint16_t index, uint16_t requested_length,
                 uint8_t response[14], uint16_t *response_length);
bool cmu_uac_alt_valid(uint8_t request_type, uint8_t request, uint16_t value,
                       uint16_t index, uint16_t length);
#endif
