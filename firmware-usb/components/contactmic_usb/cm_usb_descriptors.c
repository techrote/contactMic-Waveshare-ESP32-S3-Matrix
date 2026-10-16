#include "cm_usb_descriptors.h"
#include <string.h>
#define LO(x) ((uint8_t)((x) & 255u))
#define HI(x) ((uint8_t)(((x) >> 8) & 255u))
/* USB 2.0 + UAC2 topology, reviewed against TinyUSB 0.18.0 device/usbd.h
 * descriptor templates and Microsoft USB Audio 2.0 driver requirements.
 * Deliberately no Feature Unit: gain/DC controls are on CDC, not fake UAC controls. */
const uint8_t cmu_device_descriptor[18] = {
    18, 1, 0, 2, 0xEF, 2, 1, 64,
    LO(CMU_USB_VID), HI(CMU_USB_VID), LO(CMU_USB_PID), HI(CMU_USB_PID),
    0x09, 0x00, 1, 2, 3, 1
};
const uint8_t cmu_configuration_descriptor[CMU_CONFIG_DESC_LEN] = {
    /* Bus powered, no remote wakeup, requests up to 500 mA. Electrical compliance
     * including suspend current must be measured; LEDs/radio are not activated. */
    9, 2, LO(CMU_CONFIG_DESC_LEN), HI(CMU_CONFIG_DESC_LEN), 4, 1, 0, 0x80, 250,
    /* Audio IAD: two interfaces. */
    8, 11, 0, 2, 1, 0, 0x20, 2,
    9, 4, 0, 0, 0, 1, 1, 0x20, 2,
    /* AC total: header9 + clock8 + input17 + output12 =46. */
    9, 0x24, 1, 0, 2, 3, 46, 0, 0,
    /* Clock4: internal fixed, synchronized to USB SOF; frequency/validity read-only. */
    8, 0x24, 0x0A, 4, 5, 5, 0, 0,
    /* Microphone terminal1: mono, Clock4, no connector controls. */
    17, 0x24, 2, 1, 1, 2, 0, 4, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    /* USB streaming terminal3 from microphone1, Clock4. */
    12, 0x24, 3, 3, 1, 1, 0, 1, 4, 0, 0, 0,
    9, 4, 1, 0, 0, 1, 2, 0x20, 2,
    9, 4, 1, 1, 1, 1, 2, 0x20, 2,
    /* AS links output terminal3: type I, PCM bit0, one channel. */
    16, 0x24, 1, 3, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0,
    6, 0x24, 2, 1, 2, 16,
    /* Synchronous IN: exactly 48 mono PCM16 frames each full-speed millisecond. */
    7, 5, 0x81, 0x0D, 96, 0, 1,
    8, 0x25, 1, 0, 0, 0, 0, 0,
    /* CDC-ACM IAD, control2/data3. */
    8, 11, 2, 2, 2, 2, 0, 4,
    9, 4, 2, 0, 1, 2, 2, 0, 4,
    5, 0x24, 0, 0x20, 1,
    5, 0x24, 1, 0, 3,
    4, 0x24, 2, 2,
    5, 0x24, 6, 2, 3,
    7, 5, 0x82, 3, 8, 0, 16,
    9, 4, 3, 0, 2, 0x0A, 0, 0, 4,
    7, 5, 0x03, 2, 64, 0, 0,
    7, 5, 0x83, 2, 64, 0, 0
};
_Static_assert(sizeof(cmu_configuration_descriptor) == CMU_CONFIG_DESC_LEN, "descriptor length");

bool cmu_uac_get(uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                 uint16_t requested, uint8_t out[14], uint16_t *length)
{
    if (!out || !length || type != 0xA1 || index != 0x0400 || (value & 0xFFu)) return false;
    static const uint8_t frequency[4] = {0x80, 0xBB, 0, 0}; /* 48000 LE */
    uint16_t size = 0;
    if (value == 0x0100 && request == 1) {
        size = 4; memcpy(out, frequency, 4);
    } else if (value == 0x0100 && request == 2) {
        size = 14; memset(out, 0, 14); out[0] = 1;
        memcpy(out + 2, frequency, 4); memcpy(out + 6, frequency, 4);
    } else if (value == 0x0200 && request == 1) {
        size = 1; out[0] = 1; /* USB output clock remains valid during counted source silence. */
    } else return false;
    /* Hosts may read just the range count first. USB control truncation is valid. */
    if (!requested) return false;
    *length = requested < size ? requested : size;
    return true;
}
bool cmu_uac_alt_valid(uint8_t type, uint8_t request, uint16_t value,
                       uint16_t index, uint16_t length)
{
    return type == 1 && request == 11 && index == CMU_AUDIO_ITF && value <= 1 && !length;
}
