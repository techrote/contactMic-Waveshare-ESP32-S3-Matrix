# Project charter

## Goal

Turn a stock Waveshare ESP32-S3-Matrix into a useful wireless contact/vibration microphone by streaming an onboard QMI8658C accelerometer axis over Bluetooth Low Energy to a PC receiver.

The result should be reproducible, diagnostically transparent, and useful for listening to or recording structural vibration.

CM-009 adds an alternative wired native USB Audio Class microphone using the same
sensor/acquisition. This is additive, not a replacement for BLE. Its distinct
software and physical gates are defined in `docs/CM-009-USB-AUDIO-CONTRACT.md`.

## Product architecture

~~~text
QMI8658C
  -> deterministic single-axis acquisition
  -> bounded lock-free/nonblocking sample queue
  -> versioned BLE notification packetizer
  -> custom GATT link
  -> PC receiver
  -> gap handling / DC removal / gain / resampling
  -> 48 kHz mono output or WAV
  -> optional virtual-audio-cable route to applications expecting a microphone
~~~

The exclusive USB build instead routes the indexed queue through device-side DSP
and clock-reconciled resampling to a 48 kHz mono PCM16 UAC2 input, with correlated
CDC diagnostics/control. It needs no Bluetooth host, custom kernel driver or
virtual audio cable. Ordinary native-host behavior still requires physical testing.
No simultaneous two-consumer access to the acquisition ring is authorized.

## Explicit non-goals

- Bluetooth Classic HFP/HSP.
- Native Bluetooth LE Audio.
- Pretending the ESP32-S3 enumerates as an ordinary OS Bluetooth microphone.
- Multi-axis audio at the highest sensor ODR.
- Using the QMI8658 gyro signal as audio.
- Building a Windows kernel virtual-audio driver.
- Heavy LED animation while streaming.
- Hiding dropped sensor intervals or BLE loss.
- Simultaneous USB/BLE streaming or USB speaker playback in CM-009.

## Engineering priorities

In order:

1. truthful sampling and loss accounting;
2. stable acquisition independent of BLE or USB/CDC latency;
3. reproducible automated verification;
4. useful contact-audio bandwidth;
5. low and bounded latency;
6. simple host setup;
7. visual polish.

If the highest sensor mode is unreliable on the real board, a clean lower-rate mode is preferred over concealed sample loss.

## Target quality

Pre-hardware targets:

- firmware builds reproducibly for ESP32-S3 under the pinned ESP-IDF;
- protocol parser/encoder and DSP have deterministic host tests;
- acquisition and transport are separable by interfaces and simulators;
- BLE streams carry sequence/sample-index information sufficient to detect discontinuities;
- USB retains indexed source loss internally and exposes it through generation-correlated CDC telemetry, never custom headers inside UAC PCM;
- transport backpressure cannot stall the sensor acquisition task.

Hardware qualification targets are defined in `docs/rag/06-VERIFICATION-AND-CI.md`.

## User-facing behavior

At minimum the finished BLE repository provides:

- firmware image/build instructions;
- a host CLI that finds/connects to the device;
- live audio output to a selected host output device;
- WAV capture mode;
- readable diagnostics for source rate, sensor misses, queue overrun, BLE gaps, receiver underruns and negotiated transport parameters;
- runtime selection of at least source axis, sensor range and source-rate policy;
- documented instructions for routing output through a third-party virtual audio cable when software expects a microphone input.

The USB route adds native microphone selection, on-device processing, same-cable
stopped-state configuration, identity/status queries and a record-only qualification
runner. USB-specific defaults require USB evidence and cannot qualify BLE defaults.

## Noise and status display policy

The WS2812 matrix is a secondary feature. During active acquisition, default behavior is LEDs off. Any status display must be bounded and must not inject periodic electrical/thermal activity into the contact-mic measurement path without qualification.
