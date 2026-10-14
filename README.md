# contactMic-Waveshare-ESP32-S3-Matrix

A custom Bluetooth Low Energy contact/vibration microphone project for the
Waveshare ESP32-S3-Matrix and its onboard QMI8658C IMU.

## Status

CM-006 integrates the QMI driver, deterministic acquisition, custom GATT transport
and Python receiver/audio bridge. The firmware now prepares a real measured source
and publishes it only after the v1 session handshake. Failed sensor initialization
reports source unavailable instead of supplying synthetic samples.

**Software integration is not physical qualification.** Real-board sustained
performance, contact response and production defaults remain CM-007; release
hardening remains CM-008. AUTO starts conservatively at safe by default. A fallback
ends the current stream and requires a new connection to prepare the lower mode;
it does not silently change metadata or splice recordings midstream.

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

Tests require no devices. Discovery/audio output require the corresponding host
backends. These commands require the integrated firmware and attached hardware:

```sh
contactmic diagnostics --policy safe --duration 30
contactmic monitor --axis z --range 2 --policy safe --gain 4
contactmic record capture.wav --duration 30
```

See [development instructions](docs/DEVELOPMENT.md) for the pinned **ESP-IDF
v5.5.5 / ESP32-S3** build. See [integration and fault qualification](docs/CM-006-INTEGRATION.md)
for the native/C-to-Python tests, ownership, fallback and explicit evidence limits.
See [host instructions](docs/CM-005-HOST.md) for Windows setup, output selection,
diagnostics, recording limitations and virtual-audio-cable routing. Earlier
milestone notes describing an unavailable source are superseded by CM-006.

## Architecture and limits

```text
QMI8658C -> single-axis DRDY acquisition -> bounded indexed sample ring
  -> asynchronous source/session controller -> custom BLE GATT notifications
  -> PC loss accounting / DC removal / gain / stateful resampling
  -> 48 kHz mono live output or WAV
```

This is **not Bluetooth Classic HFP/HSP, LE Audio, or a native OS Bluetooth
microphone endpoint**. Applications expecting a microphone can receive the host
bridge through a separately installed virtual audio cable. The GATT service has
no authenticated pairing policy; use a trusted test environment.

High rate is a candidate, not a guaranteed literal 8000 Hz mode. Nominal
high/medium/safe candidates are 7174.4 / 3587.2 / 896.8 Hz in 6DOF mode; actual
measured rate is authoritative. The gyroscope is enabled but its samples are not
audio payload. The stock board's I2C interface is limited to 400 kHz. Selected-axis
two-byte reads and explicit loss accounting are essential. Software checks do not
prove the achievable interrupt/read window or RF delivery. Primary sources and
hardware contracts remain in [the RAG index](RAG.md).

## Implementation references

| Document | Contents |
| --- | --- |
| [Driver](docs/CM-002-DRIVER.md) | Register policy, vendor discrepancies and timing preconditions |
| [Acquisition](docs/CM-003-ACQUISITION.md) | Original DRDY/ring/index/rate component milestone |
| [Wire/transport](docs/CM-004-WIRE-V1.md) | Unchanged v1 bytes, shared vectors and session handshake |
| [Host](docs/CM-005-HOST.md) | Receiver, DSP, output, WAV and diagnostics |
| [Integration](docs/CM-006-INTEGRATION.md) | Current source lifecycle, AUTO boundaries and deterministic qualification |
| [Programme](docs/programme/IMPLEMENTATION-PROGRAMME.md) | Issue dependencies and acceptance sequence |

## Agent execution

Read [AGENTS.md](AGENTS.md), [RAG.md](RAG.md), the complete issue and its comments.
Reconcile current main, branches, PRs and CI; preserve existing work; implement
only the active issue. Use a dedicated PR, run available local checks and repair
CI failures. Merge only the current reviewed head with all required checks green.
Pending/running CI queries must be at least **120 seconds apart**, including
post-merge verification. Hardware-required issues need actual physical evidence.
