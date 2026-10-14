# CM-005: host receiver and audio bridge

## Present acceptance boundary

The host implementation supports protocol-v1 reception, source controls, strict
loss accounting, DSP, 48 kHz output and WAV recording. Its tests do not need a
Bluetooth adapter, audio device or ESP32. The firmware driver and GATT transport
exist, but **real acquisition is not integrated until CM-006**. The CM-004 app
reports source-unavailable; a host start against that build fails with an explicit
CM-006 message instead of producing invented microphone samples. Physical
throughput, timing, pickup quality and production defaults remain CM-007 work.

The accepted wire definitions are [CM-004-WIRE-V1.md](CM-004-WIRE-V1.md). Host
and native firmware tests consume the same canonical `tests/vectors/v1/` files.
No duplicate host-local format or renamed copy is authoritative.

## Installation

Use Python 3.11 or later, separately from ESP-IDF's Python environment. From the
repository root, Bash:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --no-cache-dir './host[ble,audio]'
python -m pip check
contactmic --help
```

PowerShell:

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install --no-cache-dir './host[ble,audio]'
$env:PATH = "$PWD\.venv\Scripts;$env:PATH"
contactmic --help
```

The base package has no mandatory runtime dependencies. Install `./host[ble]`
for BLE diagnostics/WAV without live audio, `./host[audio]` for audio APIs, or
both extras above. Live audio uses sounddevice's raw float32 output, not NumPy.
Missing libraries/adapters/output devices produce actionable errors. On Linux,
PortAudio is an operating-system dependency for live sounddevice output; install
it using the platform's package manager. Windows sounddevice wheels normally
supply PortAudio. BLE also requires a working OS Bluetooth backend, permissions
and an enabled adapter. See the primary
[Bleak installation guide](https://bleak.readthedocs.io/en/latest/installation.html)
and [sounddevice installation guide](https://python-sounddevice.readthedocs.io/en/latest/installation.html)
for platform requirements. No kernel driver or virtual microphone endpoint is
installed by this package.

CI installs both extras and checks package dependencies on Linux and Windows,
with Python 3.11 and 3.14. Tests inject devices and never scan, connect or play.
Help/version/imports do not even load the optional BLE/audio libraries.

## Commands

```sh
contactmic discover --timeout 5
contactmic audio-devices
```

Discovery matches the accepted service UUID or `S3-ContactMic` name prefix.
Choose an exact address or unique device name with `--device`. Multiple matching
devices require explicit selection; the CLI does not guess which board to use.
An address here is the platform identifier returned by discovery, not necessarily
a MAC address on every OS.

The following commands require an integrated source firmware, not the current
source-unavailable build:

```sh
contactmic diagnostics --device S3-ContactMic --axis z --range 2 --policy safe --duration 30
contactmic monitor --device S3-ContactMic --output-device 3 --gain 4 --high-pass 20 --buffer-ms 100
contactmic record capture.wav --device S3-ContactMic --axis x --range 4 --policy medium --duration 30
contactmic record pre_dsp.wav --raw --conceal-ms 0 --duration 30
```

Omit `--duration` to run until Ctrl+C. The CLI returns 2 on operational/configuration
errors and 130 on Ctrl+C. Existing WAV files are protected by exclusive creation;
only `--overwrite` permits replacing one. WAV finalization and device cleanup run
on normal exit, error and cancellation. A worker already writing a bounded audio
batch is joined before its sink is closed, rather than racing an unfinished file
write. Operating-system filesystem blocking is not a hard real-time guarantee.

Axes are x/y/z; ranges are +/-2 g and +/-4 g; requested policies are safe, medium,
high and auto. The CLI's safe default is an unqualified conservative request,
not a hardware-selected production default. Auto policy integration belongs to
CM-006; final production choices belong to CM-007. The receiver reports the
firmware's actual selected mode and measured source rate. It does not equate a
requested high policy with a guaranteed rate or transport capacity.

## Session and reconnect semantics

Controls use acknowledged writes, serialized reads and matched request IDs.
The receiver configures the source, reads a PREPARED session, installs its audio
callback, then commits that exact session ID. Audio can arrive before the commit
response; the decoder is ready before the commit is sent.

V1 audio headers have no session ID, so CM-004 allows one committed stream per
connection. Stop/reconfigure means reconnect and perform a new handshake. A
receiver run ends on disconnect instead of silently concatenating sessions.
Each run owns a distinct callback ingress generation; delayed callbacks from a
previous connection cannot mutate a later stream. Full status reads are the
lifecycle authority; delayed compact status notifications are not treated as a
reliable ordering barrier relative to control responses.

Preparation and device I/O have finite timeouts. Five seconds without an accepted
valid packet ends the run with diagnostics; duplicate/reordered packets do not
keep it alive. A source-rate or descriptor change inside a session is rejected.
Reconnect after correcting the cause rather than splicing incompatible metadata.

## Audio processing and loss

Signed accelerometer samples normalize by 32768. Default source-domain DSP is
a mild 20 Hz first-order DC blocker, linear gain 1, and hard clipping to [-1, 1].
Gain is configurable from 0 through 10000, high-pass can be disabled with zero,
and its cutoff must be below the measured source Nyquist frequency. Every clipped
sample is counted. There is no automatic gain, aggressive denoising or speech
processing. Raise range or reduce gain when clipping appears; hardware comparison
still determines useful settings.

The stateful linear interpolator keeps integer milli-Hz phase and one sample of
lookahead. Output is 48000 Hz mono float32, invariant to BLE packet partitioning.
No constant 8000 Hz assumption or packet-boundary phase reset exists. The guarded
DSP envelope is 100 through 48000 Hz input; the wire decoder preserves all valid
positive u32 rates, but pathological rates cannot cause runaway output allocation.

Small gaps may hold the previous raw source value for up to `--conceal-ms`
(default 5 ms, configurable 0–20 ms), additionally capped at 255 source samples.
All missing intervals remain counted. Larger gaps reset filter/resampler state
rather than allocating silence proportional to an attacker-controlled index.
Live output naturally underruns to silence while data is absent.

WAV stores the generated 48 kHz stream as little-endian signed PCM16. `--raw`
bypasses DC removal/gain but **still resamples to 48 kHz**; it is not an archival
native-sensor-byte recording. Set `--conceal-ms 0` to avoid held gap samples.
Large gaps are not represented by gap-sized WAV silence: its sample count is
not an elapsed-wall-clock timeline across outages. Preserve the JSON diagnostics
alongside recordings. A future archival format would need explicit gap/session
records; this version does not pretend WAV alone preserves that information.

## Bounded buffers and diagnostics

The BLE callback only copies a bounded packet into a 32-packet ingress. Queue
pressure drops oldest packets and increments `callback_queue_drops`; asynchronous
wakeups are coalesced, not queued without limit. A single serialized worker
performs decoding, DSP and sink writes outside the event loop. It never submits
an unbounded backlog of thread jobs. Public configuration caps ingress at 256
packets. Short metadata-defined gaps cannot allocate unbounded arrays.

The default live FIFO holds 4800 samples (100 ms); `--buffer-ms` accepts 1–1000 ms.
It keeps newest audio on overflow. The audio callback never waits for a
producer-held lock; it emits bounded silence and counts an underrun on contention
or missing data. Buffers bound backlog, not physical radio jitter or OS scheduling.

JSON diagnostics include named source misses, source I2C errors, ring overruns,
notification-failure observations, stack-accepted packet submissions, measured
rate, requested/selected mode, axis/range, effective MTU and known PHY/data length.
Host fields separately report packet-sequence gaps, missing source intervals,
invalid frames, rejected duplicates/reordering, concealment, large-gap resets,
callback drops, clipping and live FIFO under/overruns. Packet/source gap totals
can describe the same missing material: **do not sum them** or label every
sequence gap as RF loss. Callback drops and firmware send rejection also create
gaps. Flags identify observed fault classes; counters supply magnitudes.

Forward sequence/index deltas must be below half their modular range. Ambiguous
half-range or backward input is rejected rather than becoming an enormous gap.
The initial packet's index counts intervals absent from session origin. This
requires subscribing at the agreed start boundary, not attaching midstream.

The receiver trusts the firmware's effective MTU rather than a possibly limited
host-library property. MTU 23 carries only two v1 samples and yields an explicit
capacity warning. MTU/PHY requests are not evidence of achieved throughput.
Source misses and transport counters remain visible even where audio sounds
continuous because of concealment.

## Virtual microphone routing

This device is not HFP, LE Audio or an OS microphone endpoint. Install and
configure a virtual audio cable using its vendor's instructions. Find its playback
endpoint with `contactmic audio-devices`, select that endpoint using
`contactmic monitor --output-device ...`, then choose the cable's recording
endpoint as the microphone in the target application. Use compatible 48 kHz mono
routing or the cable's explicit channel mapping. Disable any feedback route from
the target's monitor output back into its microphone input. The package does not
install or configure third-party cable drivers.

## Reproduction and test layers

```sh
python -m pip install --no-cache-dir ./host
python -m unittest discover -s host/tests -v
cmake -S tests/native -B build/native -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native --parallel 2
ctest --test-dir build/native --output-on-failure --no-tests=error
```

Use `python -m pip install './host[ble,audio]'` to reproduce dependency resolution
from host CI. Tests cover shared audio/status/control vectors; malformed/unknown
input; signed values; sequence/index wrap and gaps; DC blocking and clipping;
packet-partition-invariant resampling; bounded FIFO/concealment; WAV headers and
exclusive creation; installed CLI/lazy imports; fake BLE preparation/commit;
source unavailable/busy/errors; old-generation callbacks; bounded wakeups; and
cancellation while output is in flight. No test is evidence of physical BLE,
PortAudio scheduling, real contact-audio quality or sustained board acquisition.
