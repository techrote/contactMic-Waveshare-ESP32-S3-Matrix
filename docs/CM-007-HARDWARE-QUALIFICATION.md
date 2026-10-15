# CM-007 hardware qualification procedure

**Status:** preparation complete; physical evidence not yet collected. This document and
`tools/cm007_qualify.py` prepare issue #7 but do not satisfy its hardware gate.

## Contents

1. Acceptance boundary — what this procedure can and cannot prove.
2. Required equipment — board, serial, BLE and audio routing.
3. Software preparation — pinned firmware and host installation.
4. Evidence runner — files, telemetry and result states.
5. Subwoofer-port fixture — repeatable relative stimulus and safety limits.
6. Preflight — identify ports/devices and verify the fixture at low level.
7. Sustained qualification — required 10-minute high and medium runs.
8. Axis/range comparison — six controlled relative-response runs.
9. Contact examples — tap, scrape and resonant-panel recordings.
10. Review and retention — thresholds, evidence selection and commit policy.

## 1. Acceptance boundary

CM-007 is the first milestone allowed to make real-board performance claims. A clean
firmware build, deterministic simulator or successful BLE connection is not physical
acceptance. Required evidence must come from a stock Waveshare ESP32-S3-Matrix with
USB serial attached and a BLE-capable host.

The subwoofer-port method below is a **repeatable relative stimulus fixture**. It is
useful for comparing axis/range choices, checking clipping/noise and exercising a
stable low-frequency signal during long runs. It is not an accelerometer calibration,
does not establish acceleration in g, and does not replace the required tap,
scraping/rubbing and resonant-object contact examples.

## 2. Required equipment

- Stock Waveshare ESP32-S3-Matrix with its onboard QMI8658C.
- Data-capable USB cable and access to the board reset/BOOT controls.
- BLE-capable host supported by Bleak. A Bluetooth adapter is required even when the
  board is connected by USB; USB is used for flash/serial, not audio transport.
- A separate host audio endpoint for ContactMic live output, preferably a virtual
  audio cable or headphones.
- For the optional fixture: a subwoofer connected to another host audio endpoint,
  a nonconductive board holder, and cable strain relief.

Do not route the ContactMic live sink back into the same speaker/subwoofer endpoint
used as the stimulus. The runner rejects identical device identifiers by default.

## 3. Software preparation

Use ESP-IDF **v5.5.5** and Python 3.11 or later. From an activated ESP-IDF shell:

```powershell
idf.py -C firmware set-target esp32s3
idf.py -C firmware build
idf.py -C firmware -p COM7 flash
```

Create a separate Python environment from an ordinary PowerShell session:

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install ".\host[qualification]"
python -m pip check
```

The qualification extra installs Bleak, sounddevice and pyserial. The ordinary
host package remains importable without touching Bluetooth, audio or serial devices.

## 4. Evidence runner

`tools/cm007_qualify.py` launches the installed `contactmic` CLI rather than a second
receiver implementation. For each run it can:

- capture timestamped serial output and the structured `CM_HW` boot identity;
- explicitly reset firmware diagnostics while stopped;
- run diagnostics, live output or raw WAV capture;
- capture one-second host/firmware status snapshots;
- play a deterministic low-frequency stimulus on a separate output device;
- evaluate the normative high/medium missed-interval thresholds;
- calculate post-startup audio underrun deltas;
- calculate raw-WAV peak, RMS and clipped-sample counts;
- write SHA-256 hashes over the complete evidence directory.

Each output directory contains some or all of:

| File | Purpose |
| --- | --- |
| `metadata.json` | command, environment, requested configuration and fixture notes |
| `serial.jsonl` | timestamped raw serial lines |
| `serial-summary.json` | board identity, boot count and crash/reset markers |
| `host.jsonl` | timestamped `contactmic` diagnostic snapshots |
| `host-stderr.log` | actionable host errors and non-JSON output |
| `stimulus.jsonl` | exact tone/silence segment transitions |
| `summary.json` | derived deltas, thresholds and result state |
| `SHA256SUMS` | evidence integrity hashes |

Result states are:

- `pass`: sustained normative gates passed with no unreviewed loss observations;
- `review_required`: normative gates passed but transport/ring/FIFO observations need
  engineering disposition;
- `evidence_complete`: a non-sustained comparison/example completed;
- `fail`: a measured acceptance criterion failed;
- `incomplete`: required serial, identity, duration, sink or telemetry was absent.

The first and last streaming snapshots define the evaluated window. Counter deltas
therefore exclude BLE preparation and initial PortAudio startup. Raw observations
remain in the evidence even when the derived result passes.

## 5. Subwoofer-port fixture

Use the port only as a controlled airflow/pressure stimulus:

1. Secure the board in a **nonconductive holder at or just outside the port mouth**.
   Do not leave it loose, insert it deeply into the enclosure, or allow it to reach
   the driver/cone.
2. Tether and strain-relieve the USB cable. Neither board nor cable may touch the
   cabinet or port edge; contact would create an uncontrolled mechanical stimulus.
3. Mark fixture geometry so every run uses the same distance, depth, board face and
   axis orientation. Photographing the fixture is useful, but large images need not
   be committed.
4. Start with the system muted and the subwoofer gain low. The script defaults to
   -36 dBFS and refuses levels above -12 dBFS. This limit does not control amplifier
   gain or acoustic output; the operator remains responsible for safe level.
5. Stop immediately if the board moves, the cable whips, the port/cabinet rattles,
   the subwoofer chuffs/distorts, or any component becomes unusually hot or smells.
6. Keep people and animals away from the port and avoid unnecessary high SPL.

`subwoofer-port-v1` loops five seconds of silence followed by tapered 25, 31.5, 40,
50, 63 and 80 Hz segments separated by silence. The exact segment times and dBFS
level are logged. Record the OS volume, subwoofer gain, crossover, phase and any DSP
settings in `--audio-settings-note`; the tool cannot read those controls reliably.

Dry-run the fixture at a very low level before connecting BLE:

```powershell
python tools/cm007_qualify.py stimulus `
  --stimulus subwoofer-port-v1 `
  --duration 90 `
  --stimulus-output-device 3 `
  --stimulus-level-dbfs -42 `
  --confirm-fixture-safe
```

## 6. Preflight

Identify the interfaces:

```powershell
python -m serial.tools.list_ports
contactmic discover
contactmic audio-devices
```

Record the BLE address, serial port, ContactMic output device and subwoofer stimulus
device. Flash the exact checkout to be tested. Start a qualification command and
press the board RESET button once when the runner waits for `CM_HW`; the structured
line records QMI address, WHO_AM_I, revision and the firmware LED policy. More than
one `CM_HW` identity during a run is treated as an unexpected reboot.

Visually confirm the LEDs are off for the baseline and pass `--led-state off`. The
firmware reports that LEDs are untouched; that statement is not a substitute for
physical observation.

## 7. Sustained high and medium qualification

The live ContactMic sink and subwoofer stimulus must use different audio devices.
The examples route ContactMic to device 7 and the stimulus to device 3. Replace all
identifiers and notes with actual values.

High, 10 minutes:

```powershell
python tools/cm007_qualify.py run `
  --output build/cm007/high-z-2g `
  --purpose sustained `
  --device "<BLE-ADDRESS>" --serial-port COM7 `
  --axis z --range 2 --policy high --duration 600 `
  --sink live --output-device 7 --buffer-ms 100 `
  --led-state off `
  --stimulus subwoofer-port-v1 --stimulus-output-device 3 `
  --stimulus-level-dbfs -36 --confirm-fixture-safe `
  --fixture-note "board clamped at port mouth; Z normal to opening; 0 mm insertion" `
  --audio-settings-note "Windows 25%; sub gain/crossover/phase fixed and recorded"
```

Medium, 10 minutes, with the fixture and audio settings unchanged:

```powershell
python tools/cm007_qualify.py run `
  --output build/cm007/medium-z-2g `
  --purpose sustained `
  --device "<BLE-ADDRESS>" --serial-port COM7 `
  --axis z --range 2 --policy medium --duration 600 `
  --sink live --output-device 7 --buffer-ms 100 `
  --led-state off `
  --stimulus subwoofer-port-v1 --stimulus-output-device 3 `
  --stimulus-level-dbfs -36 --confirm-fixture-safe `
  --fixture-note "same fixture as high-z-2g" `
  --audio-settings-note "unchanged from high-z-2g"
```

Normative targets are read from `docs/rag/06-VERIFICATION-AND-CI.md`:

- high: source-miss fraction at most 0.1%; zero I2C errors preferred; no sustained
  host audio underrun;
- medium: source-miss fraction at most 0.01%; zero I2C errors; no sustained host
  audio underrun;
- both: no crash/reboot or unbounded queue behavior.

Ring overruns, notify rejection, host packet gaps, callback queue drops and audio
FIFO overrun are not silently folded into source misses. The runner records them
as review-required observations rather than inventing an unapproved threshold.

## 8. Axis and range comparison

Use the same fixture, output level and profile for all six X/Y/Z and +/-2 g / +/-4 g
runs. Raw WAV capture makes relative peak/RMS/clipping comparisons possible without
DC-removal/gain changing the result. A 90-second example:

```powershell
python tools/cm007_qualify.py run `
  --output build/cm007/axis-x-2g --purpose axis-range `
  --device "<BLE-ADDRESS>" --serial-port COM7 `
  --axis x --range 2 --policy medium --duration 90 `
  --sink record --wav build/cm007/axis-x-2g/raw.wav --raw `
  --led-state off `
  --stimulus subwoofer-port-v1 --stimulus-output-device 3 `
  --stimulus-level-dbfs -36 --confirm-fixture-safe `
  --fixture-note "fixed port-mouth fixture; only axis/range changes" `
  --audio-settings-note "unchanged from sustained runs"
```

Repeat for `x/4`, `y/2`, `y/4`, `z/2` and `z/4`, pressing RESET once at the start of
each run. Compare `wav_analysis.peak_fraction`, `rms_fraction`, `clipped_samples`,
noise during silence segments and the raw recordings. Do not choose the production
axis/range from amplitude alone; reject clipping and unstable/noisy orientations.

## 9. Contact examples

The subwoofer fixture does not replace contact-response evidence. Capture at least:

- a finger/tap impulse on a rigid surface;
- scraping or rubbing on the contacted surface;
- a resonant panel/object response.

Example raw recording:

```powershell
python tools/cm007_qualify.py run `
  --output build/cm007/contact-tap --purpose contact-example `
  --device "<BLE-ADDRESS>" --serial-port COM7 `
  --axis z --range 2 --policy medium --duration 20 `
  --sink record --wav build/cm007/contact-tap/raw.wav --raw `
  --led-state off --stimulus-label "finger tap on rigid panel" `
  --note "three taps at approximately 5 s spacing; board held in documented position"
```

Repeat with scraping/rubbing and the resonant object. Keep files short. Subjective
fidelity may be described, but it is not a numeric acceptance metric.

## 10. Review and evidence retention

Recompute a summary without rerunning hardware:

```powershell
python tools/cm007_qualify.py summarize build/cm007/high-z-2g
```

Review `summary.json`, `serial.jsonl`, `host.jsonl` and the WAV analysis together.
Do not accept a run solely from the top-level status. Investigate every
`review_required` observation and preserve failed runs rather than overwriting them.

Raw logs and long WAVs stay under ignored `build/cm007/`. Commit only concise,
truthful summaries and small representative examples under `evidence/cm007/` after
review. Never edit raw observations to improve a result. If high fails after
reasonable correction and medium meets its stricter target, select medium as the
production AUTO default as required by issue #7.
