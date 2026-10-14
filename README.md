# contactMic-Waveshare-ESP32-S3-Matrix

A Bluetooth Low Energy contact/vibration microphone built around the Waveshare ESP32-S3-Matrix and its onboard QMI8658 IMU.

## Status

CM-001 provides the build/test foundation; CM-002 provides the QMI8658C driver.
CM-004 provides the versioned custom GATT service and nonblocking transport.
**The microphone is not integrated yet:** the app advertises with no acquisition
source and rejects stream starts rather than emitting synthetic audio. The host
CLI is still a foundation until CM-005. Real acquisition and end-to-end wiring
belong to CM-003/CM-006; physical performance remains unqualified until CM-007.
See [the wire/transport implementation](docs/CM-004-WIRE-V1.md) for exact behavior.

The repository is deliberately issue-driven. Start with [RAG.md](RAG.md), then follow the logical issue programme in [docs/programme/IMPLEMENTATION-PROGRAMME.md](docs/programme/IMPLEMENTATION-PROGRAMME.md).

## Developer quick start

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for a fresh-checkout setup,
ESP-IDF **v5.5.5 / ESP32-S3** build, Python **3.11+** installation, native tests,
and CI extension points. No attached hardware is needed for these checks.

```sh
python -m venv .venv
. .venv/bin/activate
python -m pip install ./host
python -m unittest discover -s host/tests -v
contactmic --help
```

PowerShell activation and firmware/native commands are in the development guide.
The CLI currently offers help/version only; it does not connect or stream.

## Target architecture

```text
QMI8658C accelerometer
        |
        | onboard I2C + DRDY
        v
ESP32-S3 acquisition pipeline
        |
        | custom BLE GATT notifications
        v
PC receiver
        |
        | decode -> DC removal -> gain -> resample
        v
48 kHz audio output / virtual-audio-cable input
```

This is **not** a Bluetooth Classic HFP microphone and is **not** Bluetooth LE Audio. ESP32-S3 supports Bluetooth LE, but not Bluetooth Classic, and Espressif does not provide LE Audio/ISO support for ESP32-S3. The project therefore uses a versioned custom GATT transport and a host bridge.

## Important hardware qualification

The QMI8658C Rev-A datasheet does not provide a literal 8,000 Hz accelerometer-only mode for this use. Its highest 6DOF ODR is nominally 7174.4 Hz and the accelerometer bandwidth is about 1973 Hz at that setting. That mode requires the gyroscope to be enabled.

The Waveshare schematic wires the QMI8658 over I2C:

- SDA: GPIO11
- SCL: GPIO12
- INT1: GPIO10
- INT2: GPIO13

QMI8658C I2C is specified to 400 kHz. The implementation programme therefore treats high-rate operation as a measured qualification target, not an assumption. The preferred path is direct single-axis accelerometer reads on DRDY; the design must count missed source intervals and support a lower-rate fallback if the board cannot sustain the highest ODR reliably.

## Authoritative references

See [docs/rag/00-SOURCES.md](docs/rag/00-SOURCES.md). Primary upstream sources are the Waveshare board documentation and schematic, the QST QMI8658C Rev-A datasheet, and Espressif ESP-IDF documentation.

## Agent execution

Every implementation issue is written as a complete execution handoff. Agents must:

1. reconcile current repository/issue/PR state before editing;
2. read `AGENTS.md`, `RAG.md`, and the issue-specific RAG documents;
3. implement only the issue scope;
4. run local checks;
5. push a dedicated branch and open a PR;
6. monitor CI without tight-polling — **wait at least 120 seconds between CI status checks**;
7. repair failures, then merge only after all required automated checks are green;
8. perform post-merge verification and close the issue only when its acceptance evidence is recorded.

See [AGENTS.md](AGENTS.md) for the exact execution protocol.
