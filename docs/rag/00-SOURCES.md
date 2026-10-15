# Source authority

Last reviewed: 2026-10-10. USB source extension reviewed for CM-009: 2026-10-15.

Use primary sources for hardware and platform facts. Repository contracts may deliberately select a subset or policy within vendor-supported behavior; where that happens, the repository contract governs implementation until explicitly revised.

## Waveshare ESP32-S3-Matrix

- Board documentation:
  https://docs.waveshare.com/ESP32-S3-Matrix
- Resources:
  https://docs.waveshare.com/ESP32-S3-Matrix/Resources-And-Documents
- Official schematic:
  https://files.waveshare.com/wiki/ESP32-S3-Matrix/ESP32-S3-Matrix-Sch.pdf

Verified board facts used by this repository:

- MCU: ESP32-S3 family.
- Onboard QMI8658C 6-axis IMU.
- QMI8658C signals on the schematic:
  - SDA -> GPIO11
  - SCL -> GPIO12
  - INT1 -> GPIO10
  - INT2 -> GPIO13
- The onboard sensor is wired for I2C use; implementation must not assume an accessible high-speed SPI path.
- Native USB data nets reach GPIO19/D- and GPIO20/D+; CM-009 uses the stock connector.

## QST QMI8658C

- QMI8658C Datasheet Rev A:
  https://www.qstcorp.com/upload/pdf/202210/13-52-27%20QMI8658C%20Datasheet%20Rev%20A%20(1).pdf

Verified facts relevant to this project:

- host I2C maximum clock: 400 kHz;
- highest accelerometer ODR in 6DOF mode: 7174.4 Hz;
- next rates: 3587.2, 1793.6, 896.8 Hz;
- accelerometer-only normal-mode rates top out at 1000 Hz;
- at 7174.4 Hz the default accelerometer bandwidth is approximately 1973 Hz;
- the highest rates require accelerometer + gyroscope operation;
- DRDY can be routed to INT2 in bypass/non-FIFO mode;
- the FIFO stores 6 bytes per enabled sensor sample, therefore 12 bytes per sample interval when accelerometer and gyroscope are both enabled;
- FIFO depth is programmable up to 128 samples / 1536 bytes.

Consequent repository policy: do not market or hard-code a literal 8,000 Hz source rate. Measure actual DRDY rate and report it in the stream/session metadata.

## Espressif ESP32-S3 / ESP-IDF

- ESP32-S3 BLE overview:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/ble/overview.html
- ESP-IDF versions:
  https://docs.espressif.com/projects/esp-idf/en/stable/versions.html
- ESP-IDF releases:
  https://github.com/espressif/esp-idf/releases

Platform facts:

- ESP32-S3 supports Bluetooth Low Energy.
- Bluetooth Classic is not supported on ESP32-S3.
- Espressif's current ESP32-S3 support matrix does not provide BLE ISO / LE Audio.
- This project therefore uses ESP-NimBLE and a custom GATT transport.
- Initial toolchain pin: ESP-IDF v5.5.5. A toolchain upgrade requires an explicit repository change and full CI.

## CM-009 USB stack and host authority

- Exact TinyUSB 0.18.0 source pin:
  https://github.com/hathach/tinyusb/tree/86ad6e56c1700e85f1c5678607a762cfe3aa2f47
- Pinned IDF USB PHY API (software-controlled OTG/internal-PHY device setup):
  https://github.com/espressif/esp-idf/blob/v5.5.5/components/usb/include/esp_private/usb_phy.h
- Microsoft native USB Audio 2.0 driver and supported endpoint/clock controls:
  https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers
- Espressif UAC wrapper considered as a reference, not installed as a second stack:
  https://github.com/espressif/esp-iot-solution/tree/master/components/usb/usb_device_uac

CM-009 chooses one UAC2 capture-only synchronous full-speed function plus CDC in a
single TinyUSB instance. Its exact dependency/source lock is
`firmware-usb/components/contactmic_usb/dependencies.lock.cmake`. This is not a
claim of physical Windows enumeration or USB compliance. Development VID/PID,
endpoint bytes, clock reconciliation and physical recovery are specified in
`docs/CM-009-USB-AUDIO-CONTRACT.md`. No eFuse changes or external PHY are required.

## Host libraries

Reference implementations may use:

- Bleak: https://bleak.readthedocs.io/
- NumPy: https://numpy.org/doc/
- python-sounddevice: https://python-sounddevice.readthedocs.io/
- pySerial for optional USB CDC qualification: https://pyserial.readthedocs.io/

The host architecture must keep packet parsing and DSP testable without a Bluetooth adapter or audio device. The separate USB qualification extra must not require Bleak or a Bluetooth adapter.

## Source update policy

Do not opportunistically change register interpretations or toolchain versions because a newer page exists. Open or amend an issue, record the evidence, and update the relevant RAG contract together with tests.
