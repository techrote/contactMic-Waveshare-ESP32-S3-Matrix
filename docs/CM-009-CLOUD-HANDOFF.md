# CM-009 operator handoff: cloud research/audit candidate

**Disposition: no vendor-supported acquisition fix was found. Acquisition behavior is unchanged. Do not run another 600-second attempt expecting this checkpoint to repair MEDIUM.** Build/review and offline evidence audits are useful now. Physical actions below are operator-only, conditional diagnostics; sustained qualification remains blocked until a supported correction and credible short preflight exist.

This file plus `CM-009-ACQUISITION-REVIEW.md` is the qualification addendum proposed before implementation in [#16 comment 6215982885](https://github.com/techrote/contactMic-Waveshare-ESP32-S3-Matrix/issues/16#issuecomment-6215982885). No cloud session has accessed the workstation, board, its ignored raw bundles or flashed any image.

## Exact candidate and software preparation

Use the full 40-character candidate commit from the latest cloud checkpoint on #16/#17, not the historical flashed head or an unexamined moving branch. This handoff's introducing/updating commit is the candidate anchor. In an existing repository clone, with Git available:

```powershell
$ErrorActionPreference = 'Stop'
$Candidate = (Read-Host 'Paste the full candidate commit from the #16 cloud checkpoint').Trim()
if ($Candidate -notmatch '^[0-9a-f]{40}$') { throw 'A full immutable commit is required' }
git fetch origin cm-009-usb-audio
if ($LASTEXITCODE) { throw 'Git fetch failed' }
git cat-file -e "$Candidate^{commit}"
if ($LASTEXITCODE) { throw 'Candidate is not present' }
$Anchor = (git log -1 --format=%H $Candidate -- docs/CM-009-CLOUD-HANDOFF.md).Trim()
if ($Anchor -ne $Candidate) { throw 'Handoff does not identify this candidate; reconcile the checkpoint' }
$Work = Join-Path $env:TEMP ('cm009-' + $Candidate.Substring(0,12) + '-' + [guid]::NewGuid().ToString('N'))
git worktree add --detach $Work $Candidate
if ($LASTEXITCODE) { throw 'Fresh candidate worktree failed' }
Set-Location $Work
if ((git rev-parse HEAD).Trim() -ne $Candidate -or (git status --porcelain)) { throw 'Candidate checkout is not exact and clean' }
$Session = Join-Path $Work ('build/cm009/operator-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $Session -ErrorAction Stop | Out-Null
Start-Transcript -Path (Join-Path $Session 'operator-transcript.txt')
git show -s --format=fuller HEAD
git rev-parse 'HEAD^{tree}'
py -3.11 -m venv build/host-operator
if ($LASTEXITCODE) { throw 'Host venv failed' }
$Py = Join-Path $Work 'build/host-operator/Scripts/python.exe'
& $Py -m pip install --no-cache-dir './host[usb]'
if ($LASTEXITCODE) { throw 'Non-editable host install failed' }
& $Py -m pip check
if ($LASTEXITCODE) { throw 'Dependency check failed' }
& $Py -I tools/verify_host_install.py --source host | Tee-Object (Join-Path $Session 'host-provenance.json')
if ($LASTEXITCODE) { throw 'Installed source provenance failed' }
& $Py -m pip freeze | Set-Content (Join-Path $Session 'host-freeze.txt')
& $Py -I tools/cm009_acquisition_audit.py budget | Set-Content (Join-Path $Session 'wire-budget.json')
```

Dependencies/configuration retained: ESP-IDF **v5.5.5**, target **esp32s3**, TinyUSB **0.18.0 commit `86ad6e56c1700e85f1c5678607a762cfe3aa2f47`**. The lock file Git blob is `f15b97f7b6c27e7baac3785d08c260daa742eb2f`; USB defaults blob is `17442eed8554686ceafb812028d4626bcd27957f`. Defaults: radios disabled, console disabled, 1000 Hz RTOS tick, CPU 240 MHz, power management disabled, performance optimization, AUTO ceiling SAFE. Fixed MEDIUM is selected explicitly during capture, never by changing that bootstrap ceiling. Do not change dependencies to obtain a pass.

In an activated IDF v5.5.5 PowerShell environment, retaining the variables/worktree above:

```powershell
idf.py --version
if ($LASTEXITCODE) { throw 'IDF unavailable' }
git -C $env:IDF_PATH describe --tags --always
git -C $env:IDF_PATH rev-parse HEAD
idf.py -C firmware-usb set-target esp32s3
if ($LASTEXITCODE) { throw 'USB configure failed' }
idf.py -C firmware-usb build
if ($LASTEXITCODE) { throw 'USB build failed' }
$Elf = Join-Path $Work 'firmware-usb/build/contactmic_usb.elf'
$Bin = Join-Path $Work 'firmware-usb/build/contactmic_usb.bin'
$ElfHash = (Get-FileHash $Elf -Algorithm SHA256).Hash.ToLowerInvariant()
Get-FileHash -Algorithm SHA256 $Elf,$Bin,'firmware-usb/sdkconfig','firmware-usb/sdkconfig.defaults','firmware-usb/components/contactmic_usb/dependencies.lock.cmake' |
  Select-Object Path,Hash | ConvertTo-Json | Set-Content (Join-Path $Session 'operator-build-hashes.json')
Copy-Item 'firmware-usb/build/project_description.json','firmware-usb/build/flasher_args.json','firmware-usb/sdkconfig' -Destination $Session
Select-String -Path 'firmware-usb/sdkconfig' -Pattern '^CONFIG_(BT_ENABLED|ESP_WIFI_ENABLED)=y$' | ForEach-Object { throw 'Radio unexpectedly enabled' }
```

Retain the actual ELF, binary, bootloader/partition files and flash argument files locally with hashes. Preserve build output, IDF commit/tool versions, resolved TinyUSB commit (under the build's `_deps` checkout), sdkconfig and clean Git status. The runtime must later report exactly `$Candidate` and `$ElfHash`. A rebuild at this new Git head can change provenance/image bytes even though acquisition code is unchanged. **Never reuse the historical ELF hash as the expected identity.**

## Physical readiness, fresh enumeration and recovery

Proceed only when the operator explicitly confirms the stock board is present, BOOT/RESET are identified, the data cable is available, all audio/serial applications are closed, and diagnostic flashing is intended. Stop for uncertain board identity, unusual heat/power behavior or inaccessible recovery controls. Do not replace OS class drivers, burn eFuses, increase I2C speed or modify wiring.

```powershell
if ((Read-Host 'Board present, recovery controls identified, applications closed? Type BOARD READY') -cne 'BOARD READY') { throw 'Physical readiness not confirmed' }
& $Py -m serial.tools.list_ports -v | Tee-Object (Join-Path $Session 'ports-before.txt')
& $Py -I tools/cm009_usb_qualify.py devices | Tee-Object (Join-Path $Session 'devices-before.json')
```

Operator action, not a simulated command: hold BOOT, press/release RESET, release BOOT. Re-enumerate ROM ports, correlate the physically connected board, and record the observed ROM identity. Historical ROM COM9, application COM11 and serial F0F5BD7584BC are **references only**. Read the actual MAC in ROM before flashing:

```powershell
& $Py -m serial.tools.list_ports -v | Tee-Object (Join-Path $Session 'rom-ports.txt')
$RomPort = Read-Host 'Freshly identified ROM COM port'
python -m esptool --chip esp32s3 --port $RomPort read-mac
if ($LASTEXITCODE) { throw 'ROM board identification failed' }
if ((Read-Host 'ROM identity matches the physical target? Type FLASH') -cne 'FLASH') { throw 'Flash not confirmed' }
idf.py -C firmware-usb -p $RomPort flash
if ($LASTEXITCODE) { throw 'Flash failed; preserve logs and recover through BOOT/RESET' }
```

Reset normally if required by the actual board/flash procedure, then freshly enumerate application CDC and native OS audio **before device access**. Do not substitute the ROM port for application CDC.

```powershell
& $Py -m serial.tools.list_ports -v | Tee-Object (Join-Path $Session 'application-ports.txt')
& $Py -I tools/cm009_usb_qualify.py devices | Tee-Object (Join-Path $Session 'application-devices.json')
$Serial = (Read-Host 'Fresh confirmed 12-digit uppercase board USB serial').Trim()
if ($Serial -notmatch '^[0-9A-F]{12}$') { throw 'Invalid full serial' }
$AppPort = Read-Host 'Fresh same-board application CDC port'
$AudioIndex = [int](Read-Host 'Fresh same-board native WASAPI input index')
```

Confirm the complete microphone name containing `CM USB <serial>`, application CDC serial/VID/PID, ROM MAC relationship and QMI identity in captured status. Historical QMI 0x6B/WHO 0x05/revision 0x7C must not be assumed on another board. Any difference requires review, not adapting expectations silently. Source diagnostics are valid only when all identity/provenance checks match. Observe and record LED state physically; `led_policy=untouched` is not proof LEDs are off.

## Short diagnostic and mandatory offline audit

For this unchanged acquisition candidate, a repeat short capture is justified only for a specific missing diagnostic question, not to search for a lucky passing run. Enter a reason and actual LED observation. The fresh output directory must not already exist; every retry uses a new GUID directory. Keep derived audit files **outside** the raw bundle so its manifest remains valid.

```powershell
$Reason = Read-Host 'Specific diagnostic question or approved new acquisition correction identity'
if ([string]::IsNullOrWhiteSpace($Reason)) { throw 'No justified physical attempt' }
$Led = Read-Host 'Observed LED state: off, on, or unknown'
if ($Led -notin @('off','on','unknown')) { throw 'Invalid observation' }
$Attempt = Join-Path $Session ('medium-preflight-' + [guid]::NewGuid().ToString('N'))
& $Py -I tools/cm009_usb_qualify.py capture --serial $Serial --port $AppPort --audio-device $AudioIndex --output $Attempt --purpose preflight --duration 10 --policy medium --axis z --range 2 --gain 1 --high-pass 20 --led-state $Led --wav --note $Reason
$CaptureExit = $LASTEXITCODE
& $Py -I tools/cm009_acquisition_audit.py audit $Attempt --expected-firmware $Candidate --expected-elf-sha256 $ElfHash | Tee-Object ($Attempt + '-audit.json')
$AuditExit = $LASTEXITCODE
if ($CaptureExit -or $AuditExit) { throw 'Preflight held/failed; preserve the entire attempt; do not sustain' }
```

A completed 10-second preflight does not establish the 600-second target. Review all source counters, actual interval/read rates versus preparation, `in_flight_first/last`, partition residual, positive denominator, QMI revision, firmware/ELF identity, queue/clock observations and the four elapsed durations. Account explicitly for every steady-state silence insertion, discard, overflow, short write, gap or discontinuity. Audit `review_required`, `incomplete` or `fail` all hold progression. The 3% gross rate tripwire is not a loss allowance or proof that hidden conversion loss is absent.

The capture runner already aborts on detected state/reset/USB-silence/short-write/host-buffer failures and preserves the triggering observation. This new audit is offline: it does **not** provide a new live loss-abort mechanism. Do not leave a visibly failing run going; Ctrl+C preserves the attempt. No preparation or diagnostic time contributes to evaluated duration.

## Sustained and repeated physical gates — not authorized by a clean audit alone

**The current candidate has no supported acquisition correction, so sustained MEDIUM remains blocked.** After a separately documented viable correction is implemented and reviewed, update this handoff/candidate anchor and repeat the short preflight first. Only then use a fresh directory and the same `capture` command with `--purpose sustained --duration 600 --policy medium --led-state off`. Run the same provenance-pinned audit afterward. Do not substitute SAFE or change rate/filter/thresholds to obtain a pass.

Before that sustained attempt, provide bounded live monitoring/abort disposition for source misses/I2C errors as well as USB/host faults. Stop immediately on an I2C error, reset, source/identity/generation change, unbounded queue/latency trend, sustained starvation/clock correction saturation or unaccounted conversion pause. Abort an obviously failing loss run rather than extending it to 600 seconds. Keep the triggering telemetry/control bytes and any partial recording.

Required repeated gates after a coherency change: fresh enumeration/recovery and exact runtime identity; independent free-running source/update/latch timing and exact dropped-conversion accounting; all affected axis/range configurations; close/reopen and stop/reconfigure with new generations/no stale samples; cable cycles; suspend/re-enumeration/reopen; CDC pressure alongside audio; ordinary native recording without Python/CDC; direct-contact/clipping examples; LED-off/power/electrical observations as required by the parent USB contract. Evidence must show <=0.01% actual source misses, zero I2C errors, >=600 evaluated seconds on all four clocks, and engineering disposition of every output anomaly. Instrumentation's timing effects must be separately assessed.

For each attempt preserve raw `metadata.json`, `telemetry.jsonl`, `control.jsonl`, `summary.json`, `SHA256SUMS.json`, `capture.wav` when requested, enumeration/build/flash/provenance logs and operator observations. Failed attempts are never overwritten or rewritten as successes. At session end run `Stop-Transcript`, then hash the closed transcript and other session-level files separately; do not add them inside a sealed capture bundle.

No cloud log can certify a physical button press, reconnect, LED observation or recording. Keep PR #17 draft, #16 open, BLE #7/#15 held and CM-008 unstarted until their actual acceptance requirements are met.
