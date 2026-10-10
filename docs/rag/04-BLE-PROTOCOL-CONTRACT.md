# BLE protocol contract

Protocol major version: 1.

## Transport choice

ESP32-S3 cannot provide Bluetooth Classic HFP and Espressif does not provide LE Audio/ISO for this chip. Use ESP-NimBLE with a custom GATT service.

The BLE task is transport only. It must not own sensor timing.

## UUIDs

Use the following stable UUIDs unless changed by an explicit protocol-major decision:

- Service: \`7e400001-b5a3-f393-e0a9-e50e24dcca9e\`
- Audio notify: \`7e400002-b5a3-f393-e0a9-e50e24dcca9e\`
- Control write/read: \`7e400003-b5a3-f393-e0a9-e50e24dcca9e\`
- Status read/notify: \`7e400004-b5a3-f393-e0a9-e50e24dcca9e\`

Device name prefix: \`S3-ContactMic\`.

## Session model

A BLE connection is not by itself an audio session.

Starting/restarting acquisition creates a session identifier and resets packet sequence while retaining a monotonic session-local source-sample index.

Status must expose enough information for the host to detect:

- new session;
- active axis/range/rate policy;
- measured source rate;
- sensor/queue error counters;
- effective ATT MTU when available;
- whether streaming is active.

## Audio notification v1

All integer fields are little-endian.

Header:

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | u8 | protocol_major = 1 |
| 1 | u8 | flags |
| 2 | u16 | packet_sequence |
| 4 | u32 | first_sample_index |
| 8 | u32 | source_rate_millihz |
| 12 | u8 | sample_count |
| 13 | u8 | axis/range format descriptor |
| 14 | u16 | reserved, zero in v1 |
| 16 | s16[] | samples |

Header size is 16 bytes.

Required flags:

- bit 0: a source discontinuity occurred since the previous emitted packet;
- bit 1: I2C error observed since previous emitted packet;
- bit 2: firmware sample-ring overrun observed since previous emitted packet;
- remaining bits reserved and emitted as zero.

The host must use \`first_sample_index\`, not just packet sequence, to calculate missing source intervals.

## MTU and packetization

Firmware should support ATT MTU up to 247 and request/use Data Length Extension and 2M PHY where the host permits them.

Payload size is computed from the negotiated/effective characteristic notification payload. Never overrun the negotiated ATT payload.

High-rate raw PCM is not practical at the default 23-byte ATT MTU. Therefore:

- status reports effective MTU;
- \`auto\` may reduce source rate when transport capacity is insufficient;
- host must emit a clear diagnostic when the requested source mode cannot be sustained;
- packetizer must support variable samples-per-notification without changing the v1 header.

Do not assume every OS exposes PHY or MTU negotiation identically.

## Control

Control operations must be versioned and length-checked.

Minimum operations:

- get capabilities;
- get current configuration;
- set axis X/Y/Z;
- set range +/-2 g or +/-4 g;
- set source policy high/medium/safe/auto;
- start stream;
- stop stream;
- reset diagnostic counters.

Invalid/unsupported control writes must fail deterministically and must not partially reconfigure the sensor.

## Backpressure and disconnects

BLE congestion may drop/delay transport but may not block acquisition.

If notifications cannot keep up:

- preserve acquisition counters;
- increment/report transport-drop or queue-overrun evidence;
- do not silently rewrite sample indexes;
- allow \`auto\` policy to step down only through an explicit state transition visible in status.

On disconnect:

- stop transport cleanly;
- discard stale queued audio before the next session;
- advertise again;
- retain lifetime diagnostics unless explicitly reset.

## Compatibility tests

Maintain golden binary vectors for:

- minimum valid audio frame;
- multi-sample frame;
- flags;
- sequence wrap;
- sample-index gap;
- malformed/truncated frames;
- unknown protocol major;
- control request/response examples.

The same vectors must be consumed by firmware-side/native tests and host Python tests where practical.
