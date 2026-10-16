# CM-009 dependency lock. No managed-component resolver is used for this stack.
# Upstream MIT license is retained in the downloaded checkout. Neither esp_tinyusb
# nor usb_device_uac is installed, so there is only one USB device-stack/PHY owner.
set(CMU_TINYUSB_REPOSITORY "https://github.com/hathach/tinyusb.git")
set(CMU_TINYUSB_COMMIT "86ad6e56c1700e85f1c5678607a762cfe3aa2f47") # upstream 0.18.0
set(CMU_TINYUSB_SOURCE_FILES
    src/tusb.c
    src/common/tusb_fifo.c
    src/device/usbd.c
    src/device/usbd_control.c
    src/class/audio/audio_device.c
    src/class/cdc/cdc_device.c
    src/portable/synopsys/dwc2/dcd_dwc2.c
    src/portable/synopsys/dwc2/dwc2_common.c)
