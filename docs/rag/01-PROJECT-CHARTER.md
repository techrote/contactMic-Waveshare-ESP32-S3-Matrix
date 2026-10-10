# Project charter

## Goal

Turn a stock Waveshare ESP32-S3-Matrix into a useful wireless contact/vibration microphone by streaming an onboard QMI8658C accelerometer axis over Bluetooth Low Energy to a PC receiver.

The result should be reproducible, diagnostically transparent, and useful for listening to or recording structural vibration.

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

## Explicit non-goals

- Bluetooth Classic HFP/HSP.
- Native Bluetooth LE Audio.
- Pretending the ESP32-S3 enumerates as an ordinary OS Bluetooth microphone.
- Multi-axis audio at the highest sensor ODR.
- Using the QMI8658 gyro signal as audio.
- Building a Windows kernel virtual-audio driver.
- Heavy LED animation while streaming.
- Hiding dropped sensor intervals or BLE loss.

## Engineering priorities

In order:

1. truthful sampling and loss accounting;
2. stable acquisition independent of BLE latency;
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
- every sample stream carries sequence/sample-index information sufficient to detect discontinuities;
- transport backpressure cannot stall the sensor acquisition task.

Hardware qualification targets are defined in \`docs/rag/06-VERIFICATION-AND-CI.md\`.

## User-facing behavior

At minimum the finished repository provides:

- firmware image/build instructions;
- a host CLI that finds/connects to the device;
- live audio output to a selected host output device;
- WAV capture mode;
- readable diagnostics for source rate, sensor misses, queue overrun, BLE gaps, receiver underruns and negotiated transport parameters;
- runtime selection of at least source axis, sensor range and source-rate policy;
- documented instructions for routing output through a third-party virtual audio cable when software expects a microphone input.

## Noise and status display policy

The WS2812 matrix is a secondary feature. During active acquisition, default behavior is LEDs off. Any status display must be bounded and must not inject periodic electrical/thermal activity into the contact-mic measurement path without qualification.
