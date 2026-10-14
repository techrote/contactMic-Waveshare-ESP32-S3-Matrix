# contactMic-Waveshare-ESP32-S3-Matrix

A custom Bluetooth Low Energy contact/vibration microphone project for the
Waveshare ESP32-S3-Matrix and its onboard QMI8658C IMU.

## Status

Implemented software components: the CM-001 foundation, CM-002 QMI8658C driver,
CM-003 deterministic acquisition, CM-004 GATT transport and CM-005 Python
receiver/audio bridge.

**This is not yet an integrated working microphone.** CM-003 now provides the
GPIO13 DRDY task, bounded indexed sample ring, explicit source-loss accounting and
measured-rate/policy machinery, but the firmware app still advertises with no
acquisition source attached. CM-006 connects and qualifies the complete software
path. Real-board performance and production defaults remain CM-007, followed by
CM-008 release acceptance. No synthetic samples substitute for missing hardware
or unimplemented integration.

## Developer quick start

Use Python 3.11+ from the repository root:

```sh
python -m venv .venv
. .venv/bin/activate
python -m pip install './host[ble,audio]'
python -m unittest discover -s host/tests -v
contactmic --help
contactmic discover
contactmic audio-devices
```

Tests require no devices. Discovery and audio-device listing require their
respective OS backends. Stream commands will need CM-006 integrated firmware:

```sh
contactmic diagnostics --policy safe --duration 30
contactmic monitor --axis z --range 2 --policy safe --gain 4
contactmic record capture.wav --duration 30
```

See [development instructions](docs/DEVELOPMENT.md) for the pinned **ESP-IDF
v5.5.5 / ESP32-S3** build and native tests. See [host instructions](docs/CM-005-HOST.md)
for Windows setup, output selection, diagnostic counters, recording limitations
and virtual-audio-cable routing.

## Architecture and limits

```text
QMI8658C -> single-axis DRDY acquisition -> bounded sample source
  -> custom BLE GATT notifications -> PC receiver
  -> loss accounting / DC removal / gain / stateful resampling
  -> 48 kHz mono live output or WAV
```

This is **not Bluetooth Classic HFP/HSP, LE Audio, or a native OS Bluetooth
microphone endpoint**. Applications expecting a microphone can receive the host
bridge through a separately installed virtual audio cable.

High rate is a candidate, not a guaranteed literal 8000 Hz mode. The QMI8658C
Rev-A high/medium/safe candidates are nominal 7174.4 / 3587.2 / 896.8 Hz in 6DOF
mode. The gyroscope must be enabled although its samples are not audio payload.
Actual measured source rate is carried in protocol metadata. The stock board's
I2C interface is limited to 400 kHz; single-axis two-byte reads and explicit loss
accounting are essential. Source/transport/host loss is never silently erased to
make the stream look reliable. Primary sources and the hardware contract remain
in [the RAG index](RAG.md).

## Implementation references

| Document | Contents |
| --- | --- |
| [Driver](docs/CM-002-DRIVER.md) | Register policy, vendor discrepancies, lifecycle and timing preconditions |
| [Acquisition](docs/CM-003-ACQUISITION.md) | ISR/task ownership, source-index gaps, bounded ring, measured rate and policy |
| [Wire/transport](docs/CM-004-WIRE-V1.md) | V1 bytes, shared vectors, session handshake and nonblocking ownership |
| [Host](docs/CM-005-HOST.md) | Receiver, DSP, bounded output, WAV, diagnostics and CLI |
| [Programme](docs/programme/IMPLEMENTATION-PROGRAMME.md) | Issue dependencies and acceptance sequence |

## Agent execution

Read [AGENTS.md](AGENTS.md), [RAG.md](RAG.md), the complete issue and its comments.
Reconcile current main, branches, PRs and CI; preserve existing work; implement
only the active issue. Use a dedicated PR, verify available local tests, repair
CI failures, and merge only the current reviewed head with all required checks
green. Pending/running CI queries must be at least **120 seconds apart**, including
post-merge verification. Record exact acceptance evidence; hardware-required
issues cannot be accepted without actual physical measurements.
