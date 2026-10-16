# CM-009 Windows qualification record — 2026-10-16

Disposition: **USB hardware qualification incomplete / BLE hardware held**.
Issue #16 remains open and PR #17 remains draft. No native microphone acceptance,
high/medium result, lifecycle pass, contact response or production default is
claimed by this checkpoint.

## Reconciliation and workstation

- Existing branch: `cm-009-usb-audio`; starting head
  `37f6bff93c1c249531d1c6733157cc4577f1fac5`, tree
  `fd132c5f346a513dce2da4afc813fc08ae25777c`.
- Accepted main: `0d4076131f9ff699b44a1eb3bcf8fcf95f99c93d`.
- Starting-head CI `38815282174`: eight successful jobs; no unresolved inline
  review threads. This historical CI does not qualify the corrections below.
- Fresh complete checkout: `C:\espmic`; no previous local changes or physical
  evidence. CM-007/#7, CM-008/#8 and draft PR #15 remain separate and held.
- Windows 11 Pro, version `10.0.26300`, build `26300`, x64.
- Separate non-editable Python 3.12.10 host environment:
  `build/cm009-host/`; imports resolve to its `Lib/site-packages/contactmic`.
  USB dependencies: sounddevice 0.5.6, pyserial 3.5, cffi 2.1.1, pycparser 3.11.
  `pip check` passes. No BLE receiver or speaker playback was launched.

## Positive board identification and recovery

The operator identified the initially enumerated HID/CDC `TM-002` on COM10 as
the intended stock Waveshare ESP32-S3-Matrix and confirmed physical readiness.
They performed and confirmed: hold BOOT, press/release RESET, release BOOT.
Windows then exposed ROM USB-Serial/JTAG on COM9, VID:PID `303A:1001`, serial
`F0:F5:BD:75:84:BC`. A scoped esptool identity query confirmed:

- ESP32-S3 QFN56 rev 0.2, 40 MHz crystal;
- factory MAC `F0:F5:BD:75:84:BC`;
- embedded XMC 4 MB flash and AP_3v3 2 MB PSRAM.

The original 4,194,304-byte flash image was backed up before replacement:
`build/cm009/session-20261016/original-TM-002-flash.bin`, SHA-256
`ae5461b77b3ca26049e283c85036bb1b852460bffe8f61161ef5783fac80968a`.
The identity and backup reads used existing esptool 5.5.0 from a separate local
IDF 6.1 tools environment; neither that IDF nor its compiler built this firmware.
No eFuse/security changes, full flash erase, rewiring or driver replacement occurred.

## First USB binary and physical execution

ESP-IDF v5.5.5 at `b774170ff46c393eeb5e495ea37936038d3f4f4f` and its installed
Xtensa GCC 14.2.0 `esp-14.2.0_20260121` built the isolated USB project.
TinyUSB is pinned to `86ad6e56c1700e85f1c5678607a762cfe3aa2f47` (0.18.0).
USB sdkconfig disables radios, selects 240 MHz/1000 Hz tick, no console and safe
AUTO ceiling. The conservative 2 MB flash layout fits the detected 4 MB chip.

The first binary used unchanged firmware source from the starting head. Two host
files were locally modified, so IDF recorded project version `37f6bff-dirty`.
`build/cm009/session-20261016/build-001.json` records the source, local changes,
dependency identities and hashes. Copies of the ELF/bin and effective sdkconfig
are retained in that directory.

| Artifact | SHA-256 |
| --- | --- |
| First ELF | `6960c26316906ec2b4bc33866078a68e32c7af9c3fc1f12dcc336252055d0b9f` |
| First application binary | `e66f9f9d73b11edf36ca6b890767f78021089e67d3ea0ea36e8fa1022ee0cb88` |
| First sdkconfig | `de97e99b9bce9c6c6d5b6036e8a5117cab1161b045ad6cb8fe7118f5e8d71521` |

`idf.py -C firmware-usb -p COM9 flash` completed and verified the written regions.
Subsequent enumeration still showed ROM/JTAG, with no UAC/CDC application. Normal
RESET with BOOT released and LED/safety observations have been requested from the
operator; its result is pending at this checkpoint. Flash success is not an
application-startup or sensor pass. QMI identity and the runtime ELF query remain
unobserved. The later corrected build is not this flashed binary.

## Audit corrections and deterministic evidence

1. The pinned TinyUSB `audio_device.c` passes `idx_audio_fct=0` to
   `tud_audio_tx_done_pre_load_cb`. The adapter erroneously compared that argument
   with AudioStreaming interface number 1; the fake repeated this error. Correcting
   the fake first fails against the original adapter at the first preload. The
   adapter now accepts function 0 and rejects wrong function/endpoint/alternate.
2. The host runner discarded status observations which triggered startup or
   generation/identity aborts. A failed-source regression first demonstrated an
   empty telemetry log. It now preserves the triggering row and timestamped raw
   CDC request/response fragments before validation, including malformed replies.
   Observed steady silence/short writes abort promptly and retain the attempt.

These are software findings; no fabricated physical failure rate or audio result
is attributed to them. Pre-fix and repaired logs remain under the raw directory.

Actual local verification from the complete checkout:

- Baseline: 67 installed host tests, 8 tooling tests, 13 native CTest targets,
  7 CM-006 production-C-to-installed-host integration tests passed.
- Initial environment failures are retained: host CLI missing from process PATH
  and CMake unavailable. Correcting PATH/tool availability resolved them without
  changing test expectations.
- Corrected host: 70 tests passed after non-editable reinstall; `pip check` passed.
- Corrected firmware: all 13 native tests passed, including actual adapter/core/
  acquisition and six 600-second **synthetic** independent-clock cases.
- Both USB and retained BLE firmware built locally under pinned ESP-IDF v5.5.5.
  Native Windows tests do not include the Linux-only pthread stress target.

Raw logs, Windows/PnP enumeration, build/flash identities, original flash backup
and binaries: `build/cm009/session-20261016/` (ignored, local evidence).
No sustained capture has started. Physical preflight, ordinary Windows recorder,
high/medium matched windows, lifecycle/CDC/source-failure checks, LED observation,
six axis/range comparisons, contact examples and defaults remain outstanding.
Electrical/bus measurements and acceleration calibration are unavailable, not zero.

## Subsequent operator reset and first native enumeration

The operator confirmed a normal RESET with BOOT released, LEDs off, and no unusual
heat/smell/USB warning. Windows then enumerated `CM USB F0F5BD7584BC` as the UAC2
microphone and application CDC COM11, development VID:PID `CAFE:4014`. Microsoft's
signed `usbaudio2.sys` / `usbaudio2.inf` version `10.0.26100.9457` and `usbser.inf`
version `10.0.26100.9278` bind those functions. This is real native enumeration;
ordinary application recording and source performance are still unqualified.

The first CDC query returned QMI address `0x6B`, WHO_AM_I `0x05`, revision `0x7C`,
source available and stopped. Its firmware identity matched `37f6bff`, but ELF
identity was only `6960c2631`. The runner correctly refused the truncated hash.
Raw reply bytes are preserved in `first-binary-control.jsonl` and
`first-binary-reply.json`; no passing capture was produced.

Root cause: pinned IDF's `esp_app_get_elf_sha256()` copies at most the configured
string length, default nine characters, even with a 65-byte caller buffer. The
old native fake incorrectly promised all 64. A fake modeling the actual default
first failed the 64-character regression. The adapter now converts all 32 raw
bytes from the linked app descriptor, and the regression checks the complete
known byte sequence. All 13 native tests and the actual USB build pass after this
correction. No hash check or acceptance threshold was weakened.

The operator then confirmed recording applications closed and a fresh BOOT/RESET
recovery for flashing the corrected firmware. The first firmware image, binary
identities and failed reply remain preserved.

## First ordinary native recording and failed preflight

Clean head `532cfe85954db64fc2961e005353c0162ef4795c` was flashed and queried.
Runtime full ELF hash matches saved build `binary-005/`:
`2f547c6d91d97753a16cb870ad4e4cf8eac364d043d95f54c618da73b38754b7`.
Application binary SHA-256:
`bac02e9eaaccf232b87976d2584ca2781555d58d708a9d9d4d56635d1106d573`.
Exact-head CI `38879786487` passed all eight jobs. Operator confirmed normal
RESET, matrix off and no unusual heat/smell/USB warnings.

The operator placed the board in contact with the subwoofer body and confirmed
secure nonconductive insulation, strain relief and absence of movement/chuffing/
distortion/heat. They authorized recording the microphone and PC playback. Existing
music at the user's unchanged level is relative excitation only. Cabinet contact
is the recorded fixture; no port/airflow or calibrated acceleration claim is made.

FFmpeg 9.0.2 (Gyan release essentials, linked from FFmpeg's download page; archive
checked against its published SHA-256) recorded the exact DirectShow microphone
name, with CDC closed and no repository receiver. It negotiated native 48 kHz mono
PCM16. The saved WAV has 575,712 frames; after excluding the first two seconds,
its RMS is only 0.479 PCM counts and peak one count. **This is not meaningful
contact pickup or a successful ordinary-microphone acceptance.** Post-recording
CDC retained boot ID `2181138798` but reported zero source intervals/reads and
599,232 failed-silence output samples, with no short writes or host dshow buffer
failure reported. Software USB output does not establish working acquisition.

Raw ordinary recording: `build/cm009/ordinary-music-001/native-microphone.wav`,
SHA-256 `17ffca780a8605d70c7d61f5c8dc429996f514eae2ec3eeebfa8c4fe1343a0f5`.
The directory also retains recorder/version/driver selection logs, statistics
and post-recording CDC bytes/status. A separate eight-second speaker-loopback
recording confirmed active PC output (peak 0.502, RMS 0.153 of digital full scale,
no API warning). It was not simultaneous with the first microphone recording;
it is not a timing, acoustic-level or acceleration reference.

`build/cm009/preflight-001/` then failed after about one second, with the failed
startup observation preserved. State PREPARING became FAILED / BACKEND; actual
source intervals and reads remained zero. The requested ten-second evaluated
window never began, so the miss fraction has no valid denominator. Host overflow,
queue drops and bad-block counts were zero in this short attempt; those facts do
not turn it into a pass. Evidence manifest SHA-256:
`26590ef0ad462f3f0189ccf955e91b4ce0f8c83996eba14285736b13b2e410a6`.
No sustained high/medium capture was attempted on this failed chain.

The next scoped diagnostic adds an atomic retained backend-failure stage/error
to distinguish configure/start failures from a completed zero-edge measurement.
It adds no I2C to USB callbacks and does not change driver registers or thresholds.
The native adapter tests cover failure stages and retention across cleanup; all
13 native tests and the pinned USB build pass. The operator confirmed recovery
for this diagnostic build. Further physical results remain pending.
