# Hardware contract

## Board

Target: Waveshare ESP32-S3-Matrix, SKU 27119.

The official schematic is the authority for board-level wiring.

## QMI8658C connection

The onboard QMI8658C is connected as follows:

| Signal | ESP32-S3 GPIO | Use |
| --- | ---: | --- |
| IMU_SDA | 11 | I2C data |
| IMU_SCL | 12 | I2C clock |
| IMU_INT1 | 10 | optional control/handshake interrupt |
| IMU_INT2 | 13 | primary DRDY / FIFO interrupt |

Firmware must probe both legal QMI8658C I2C addresses rather than assuming one from third-party sample code, then validate WHO_AM_I before configuration.

I2C clock must not exceed the QMI8658C datasheet maximum of 400 kHz.

## High-rate consequence

The QMI8658C Rev-A highest accelerometer ODR is 7174.4 Hz in 6DOF mode. The gyroscope must therefore be enabled even though its samples are not useful to the contact-mic stream.

At that mode:

- source interval is roughly 139 microseconds;
- accelerometer default bandwidth is about 1973 Hz;
- the board's 400 kHz I2C connection is the dominant acquisition constraint.

A two-byte direct read of one accelerometer axis per DRDY is the preferred high-rate experiment.

Do not read all accelerometer + gyro data at every source interval on the high-rate path.

## FIFO policy

The sensor FIFO is not the default high-rate solution.

With accelerometer and gyro both enabled, FIFO storage is 12 bytes per source interval. At 7174.4 Hz that sensor payload alone is about 86 kB/s before I2C protocol overhead, exceeding what a 400 kHz I2C bus can carry.

FIFO may still be used for lower-rate experiments if an issue demonstrates a benefit and preserves protocol semantics.

## Axis and range

All three accelerometer axes must be supported.

Initial default may be Z (normal to the PCB), but final production default is chosen by hardware qualification rather than assumption.

Required ranges:

- +/-2 g for maximum sensitivity;
- +/-4 g as the first anti-clipping fallback.

Additional ranges may be exposed if low-cost.

## Sensor register ownership

The sensor driver owns QMI8658 configuration. Other components must not write QMI registers directly.

The driver must expose typed operations for:

- reset/probe;
- configure rate/range/axis-read mode;
- start/stop;
- read selected axis;
- retrieve identification/revision information;
- retrieve or increment low-level error counters.

Register constants must cite the datasheet section/table in comments where practical.

## LEDs

The onboard matrix is not required for acquisition correctness.

Streaming default: matrix off.

If status LEDs are added, they must be rate-limited and hardware-qualified for noise/thermal effects. Do not continuously refresh animations during audio streaming.

## Hardware claims

Simulator tests can prove software behavior but not:

- achievable DRDY service rate;
- actual I2C transaction timing;
- RF throughput on a particular host;
- contact pickup sensitivity;
- board self-noise.

Those claims belong to the hardware qualification issue.
