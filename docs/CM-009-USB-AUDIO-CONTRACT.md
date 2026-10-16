# CM-009: native USB microphone software and qualification contract

## Status and scope

This is the **software implementation / USB-hardware-held** path. The USB project
implements an actual UAC2 microphone, not serial PCM or a virtual-audio bridge.
Physical Windows enumeration, electrical behavior, long capture, contact response
and production defaults are not established by its source or automated tests.
Issue #16 and its PR remain open/draft until the physical gates are met. CM-007/#7
BLE hardware qualification and its draft PR #15 remain separate and held.

`firmware/` remains the accepted BLE application, unchanged. `firmware-usb/` is an
isolated USB-only ESP-IDF project sharing the accepted QMI/acquisition components.
It has a separate sdkconfig/build directory and does not initialize or enable a
radio. Simultaneous transports and a second consumer of the SPSC ring are excluded.
The native OS microphone works without the Python receiver, CDC being open, a
Bluetooth adapter, a virtual audio cable, or an application-specific kernel driver.

## Descriptor and stack decision

Use ESP-IDF **v5.5.5** and one directly compiled upstream TinyUSB **0.18.0** instance,
commit `86ad6e56c1700e85f1c5678607a762cfe3aa2f47`. The checked CMake dependency lock
lists both that commit and the exact source subset. FetchContent retrieves the
upstream checkout into the ignored build tree; configuration verifies its Git
identity. No independently initialized esp_tinyusb/usb_device_uac stack is installed.
The upstream MIT license stays in that downloaded checkout.

The esp-iot-solution `usb_device_uac` README was considered as a reference; its
published Windows/Linux limitations and the need for a composite CDC function are
reasons not to use that wrapper blindly. Relevant primary sources:

- TinyUSB pin: <https://github.com/hathach/tinyusb/tree/86ad6e56c1700e85f1c5678607a762cfe3aa2f47>
- Microsoft UAC2 support and fixed-clock controls:
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers>
- Espressif UAC reference:
  <https://github.com/espressif/esp-iot-solution/tree/master/components/usb/usb_device_uac>
- Pinned software PHY API:
  <https://github.com/espressif/esp-idf/blob/v5.5.5/components/usb/include/esp_private/usb_phy.h>

The descriptor graph is UAC2 AudioControl interface 0, AudioStreaming interface 1
(alt 0 stopped, alt 1 capture), and CDC control/data interfaces 2/3. Clock entity 4
is fixed at 48,000 Hz and synchronized to USB frames. Microphone terminal 1 feeds
USB streaming terminal 3. The audio IN endpoint is 0x81, synchronous, full-speed,
96 bytes per 1 ms service interval: **48 samples, mono, signed little-endian PCM16**.
CDC uses notification IN 0x82 and bulk OUT/IN 0x03/0x83. There is no speaker output,
feedback endpoint, high-speed descriptor, or unimplemented feature-unit control.
Clock GET_CUR/GET_RANGE and validity are implemented; a fixed-clock SET_CUR stalls.
The output clock stays valid during explicitly counted source-unready silence.

The 193-byte configuration contains a 118-byte audio function and a 66-byte CDC
function. Endpoint allocation fits the S3 full-speed controller. The bus-powered
configuration requests at most 500 mA and advertises no remote wakeup. **Actual
power, suspend current and USB electrical compliance require physical measurement**;
there is no certification claim. LEDs are untouched and their off state must be
observed, not inferred from a log message.

VID `0xCAFE`, PID `0x4014` are an explicit **TinyUSB-style development identity**,
not a claimed allocation owned by this project. Do not distribute a commercial
product under this identity without arranging a legitimate VID/PID policy. The
USB serial is the 12-digit uppercase factory MAC identity (read only, no eFuse
writes); the product/interface name `CM USB <serial>` binds host audio selection
to the same CDC serial. Firmware Git and ELF SHA-256 identity are queryable.
The adapter encodes all 32 ELF-hash bytes from `esp_app_get_description()`;
IDF's string helper is limited by `APP_RETRIEVE_LEN_ELF_SHA` (nine characters by
default) and is insufficient for the qualification identity.

## USB PHY, build and recovery

Use the stock native USB connector, GPIO19 D- and GPIO20 D+. Application
`usb_new_phy` selects internal USB-OTG device ownership in software. The fixed
hardware USB Serial/JTAG port must not be assumed to coexist on that PHY. CDC is
provided by this same application TinyUSB instance. No ROM CDC console stack is
mixed in. No eFuse, security configuration or permanent PHY change is required.

From an activated **ESP-IDF v5.5.5** shell at the repository root:

```powershell
idf.py --version
idf.py -C firmware-usb set-target esp32s3
idf.py -C firmware-usb build
```

`set-target` is for a clean initial configuration; subsequent incremental work uses
`build`. BLE still builds independently with `idf.py -C firmware build`. USB's
sdkconfig disables Bluetooth/Wi-Fi, uses the 1000 Hz FreeRTOS tick and does not
route the console through a competing USB controller. Configure the USB-specific
AUTO ceiling with `CONTACTMIC_USB_AUTO_CEILING`; default **safe** remains an
unqualified bootstrap setting, not an evidence-selected production default.

Before flashing, locate BOOT/RESET and retain a data-capable cable. The recovery
procedure to verify physically is: hold BOOT, press/release RESET, then release
BOOT; identify the ROM download COM port; flash from that port, then reset normally:

```powershell
idf.py -C firmware-usb -p COM7 flash
```

COM/audio identifiers can change between ROM and application modes. Close all
recording/serial applications before flashing. Never use Zadig/WinUSB to replace
the native audio class driver, and never burn USB_PHY_SEL or other eFuses as a
workaround. Failed early USB initialization may leave no application CDC output;
recover through BOOT/RESET rather than assuming the board is permanently lost.

## Ownership and lifecycle

The accepted acquisition task remains the sole producer. USB is the only external
consumer in this build. USB callbacks perform bounded source consumption, DSP and
stack copies; they never configure/read I2C or wait for settling. A lower-priority
controller owns all sensor lifecycle calls. Atomic 32-bit generation-tagged
mailboxes have one writer each and bounded snapshot attempts, not spin loops.

An AudioStreaming start posts asynchronous preparation. The accepted acquisition
adapter drains calibration data, measures ISR-observed rate, then acknowledges a
fresh publication/index origin. USB renders counted startup silence until that
generation is ready and its source reserve is primed. The controller has two-second
measurement/publication budgets; a frontend budget also detects a controller that
never runs. A failed source remains diagnosable and generates **failed silence**,
not synthetic valid sensor samples. Controls cannot reconfigure an open capture.

Stop gates the consumer before asynchronous shutdown. Reopen creates a new
generation without requiring physical unplug. Reset/detach invalidation prevents
old transfer callbacks reopening a source before a fresh mount. The pinned stack
calls its first audio preload before the final SET_INTERFACE callback; start is
idempotent across that ordering. The preload callback's second argument is the
zero-based audio **function ID** (0 for this build), not AudioStreaming interface
number 1. The adapter regression uses the pinned stack's actual call convention.
Source queues/DSP history are not reused across
these boundaries; lifetime diagnostics survive them.

Suspend deliberately gates and disconnects the application USB function, clears
its audio FIFO, then requests re-enumeration. It does **not** resume a queued old
transfer. The host must reopen recording following re-enumeration; seamless
suspend continuation is not promised. This conservative lifecycle and the board's
actual suspend power/resume behavior are physical acceptance checks, not results
established by mocks. Hardware/OS-specific unsupported behavior must remain held.

## DSP, source gaps and independent clocks

Source rate is the measured DRDY rate, not 48 kHz and not a literal 8 kHz promise.
The external USB output rate says nothing about accelerometer bandwidth. Processing
is a configurable source-domain DC blocker, linear gain and saturating limiter,
then stateful linear interpolation. Defaults are gain 1 and cutoff 20 Hz. Zero
cutoff and gain 1 provide the pre-DSP/native-amplitude diagnostic route, still
resampled to 48 kHz. Range changes alter raw sensitivity; normalized PCM amplitude
is not calibrated acceleration in g.

The USB-host and sensor clocks are independent. The resampler starts from the
measured ratio, then a bounded PI occupancy servo adjusts source phase advance.
The target reserve is 20 ms of source data (8..192 entries); correction is limited
to +/-3%. Corrections, saturation and queue high-water marks are reported. This is
rate conversion, not deletion/duplication passed off as sensor observations. Tests
run all three nominal candidates for ten simulated minutes at both -0.5% and
+0.5% source-clock error, and exercise MCU callback jitter and delayed service.
This validates bounded numerical behavior, **not physical RF/USB/CPU throughput**.

Indices remain intact internally. Missing intervals up to min(20 ms,32 intervals)
are represented by counted source-domain zeros; larger gaps reset/reprime with
bounded work. No held sample conceals indefinite loss. All output priming,
starvation and failure silence is counted separately. After first RUNNING, any
repriming silence is starvation, not renamed startup. Queue pressure above 224
entries explicitly discards toward the reserve and increments its own counter.
Raw rail observations, DSP clipping, source faults and resampler resets remain
visible. USB-short-write and callback-delay observations are not claimed to count
every physical USB bus error. Different counters can describe overlapping losses;
do not add them together as independent missing material.

AUTO is USB-specific: new source errors in an observed one-second window request
high -> medium -> safe through a new asynchronous source generation. The OS's
external format remains 48 kHz. Safe failure becomes an explicit failed state.
Fixed policies do not silently downgrade. Native applications may continue an
open UAC recording across the counted gap, but evidence capture rejects a changed
generation. No BLE mode/default is changed by USB observations.

## Same-cable diagnostics and controls

CDC is a bounded, polled, newline-delimited ASCII request/JSON reply channel:

```text
STATUS 1
CONFIG 2 2 2 1 1000 20000
```

CONFIG fields are request ID, axis (0=X,1=Y,2=Z), range (2 or 4 g), policy
(0=safe,1=medium,2=high,3=auto), gain in thousandths (0..64000), and cutoff in
milli-Hz (0..100000). Configuration is atomic at the frontend and rejected while
an OS capture is open. There is no implicit lifetime-counter reset. The complete
status/identity can be queried after opening CDC even when boot text was missed.

Replies use `cm009.v1`, correlate `request_id`, and include stable serial, boot ID,
reset reason, firmware/ELF identities, QMI identity, generation/state/reason, mode,
rate/configuration, output position, source counters, USB/DSP counters and queue/
clock observations. `source_snapshot_fresh=false` means a contended source-counter
snapshot; it is not exact current evidence. Unknown QMI is null. LED policy
`untouched` is not a physical LED-off observation. State values: stopped 0,
preparing 1, priming 2, running 3, failed 4. Result values: OK 0, invalid 1,
busy 2, I/O 3, generation exhausted 4.

The parser accepts fewer than 128 bytes per line; CDC processes at most 64 received
bytes and 64 transmitted bytes per pump, with one bounded 3072-byte pending reply.
RX/TX-completion callbacks service CDC even when continuous audio keeps the USB
event loop busy. Clients assert DTR (the runner does); application CDC DTR does
not drive BOOT/RESET. Closing CDC clears its parser/reply state without stopping
audio. Flooded, malformed, overflowing or unread traffic is counted/dropped, never
waited on by audio. Both USB FIFO and controller allocations have fixed boot lifetime.

## USB-only host runner

Use a separate Python 3.11+ environment and install the ordinary package's USB
extra. It contains sounddevice and pyserial, **not Bleak**:

```powershell
python -m pip install ".\host[usb]"
python tools/cm009_usb_qualify.py devices
```

The runner requires a full serial match in both CDC metadata and the audio product
name. Windows WASAPI is preferred over duplicate MME/DirectSound aliases. Ambiguous
or truncated names are rejected; select exact `--port` and `--audio-device` when
needed. No default microphone guessing or BLE discovery/reset occurs. CDC identity
is queried again before capture. Record actual Windows driver/device descriptors
and normal-application selection separately for the physical report.

Close other recorders and sound-input meters first, then run a short **record-only**
preflight (replace the example serial and indices):

```powershell
python tools/cm009_usb_qualify.py capture --serial AABBCCDDEEFF `
  --port COM7 --audio-device 4 --output build/cm009/preflight `
  --purpose preflight --duration 10 --axis z --range 2 --policy safe `
  --led-state off --wav --note "LED state and fixture physically observed"
```

After confirming enumeration, meaningful contact pickup, controls and recovery,
run fixed high and medium with `--purpose sustained --duration 600`, separate new
output directories, and observed LEDs off. Start/end of the evaluated window are
RUNNING snapshots of one generation. Device elapsed time, host monotonic time,
USB output samples and captured OS samples must each cover the requested duration.
Preparation is not counted toward that duration. All raw observations remain.

The runner captures native 48 kHz mono PCM16 with no live output. WAV is optional
because long files are large. Callback ingress is bounded; a disk/consumer stall,
host input overflow or malformed block aborts rather than fabricating complete
recordings. Telemetry gaps above 3.5 seconds invalidate continuous evidence; this
is a measurement-coverage rule, not a relaxation of a hardware loss threshold.
Source miss fractions use matched source-interval counter deltas, never USB output
sample count. Boot/generation/rate/config changes, saturation/regression, stale
counter snapshots, missing telemetry, cleanup failure and short capture prevent
an automatic pass.

High's source-miss limit remains 0.1% (zero I2C errors preferred), medium's 0.01%
(zero I2C errors required). Steady output starvation/failure silence, short writes
or host overflow fail. Other loss, clock-saturation, gap-fill, clipping and queue
observations require review rather than automatic success. A run-level pass never
sets `issue_accepted=true` and never replaces the remaining physical programme.

Each new directory preserves metadata, raw timestamped telemetry, summary,
optional WAV and SHA-256 hashes. `control.jsonl` additionally preserves timestamped
CDC request/response byte fragments as hex before parsing/validation, including
initial/configuration/shutdown exchanges and rejected or malformed replies.
Telemetry retains the status which triggers an abort. Observed steady-state
silence or short writes abort the attempt promptly instead of continuing an
already-failed run for its full requested duration. Existing directories are never recursively
removed or overwritten. Re-evaluation verifies hashes and does not rewrite files:

```powershell
python tools/cm009_usb_qualify.py summarize build/cm009/preflight
```

Keep raw runs under ignored `build/cm009/`. Publish only reviewed concise summaries
and useful small examples under `evidence/cm009/`. A hash proves integrity of a
recorded bundle, not that a synthetic test was a physical observation.

## Qualification and remaining physical gate

Normal CI builds both isolated firmware projects, the actual core/descriptor tests,
the actual USB adapter against explicit external API fakes, the unchanged production
acquisition core feeding USB PCM under faults, and the installed host
runner's fake-device and false-pass regressions. The unchanged BLE protocol, shared
acquisition, CM-006 integration and existing host tests remain required.

Focused offline reproduction:

```sh
cmake -S tests/native/cm009 -B build/cm009-native -DCMAKE_BUILD_TYPE=Release
cmake --build build/cm009-native --parallel 2
ctest --test-dir build/cm009-native --output-on-failure
python -m pip install ./host
python -I -m unittest discover -s host/tests -p test_usb_qualification.py -v
```

Native fake drivers do not prove USB enumeration, electrical/suspend behavior or
physical DRDY timing. Before accepting issue #16, use Windows' native UAC driver
and an ordinary recorder without this runner, prove BOOT/RESET recovery, complete
both sustained modes, ten close/reopen cycles, five unplug/replug cycles, supported
suspend/resume, closed/slow CDC, six axis/range comparisons and tap/scrape/resonant
contact examples. Preserve failures and choose USB-specific defaults from evidence.
BLE acceptance remains held. CM-008 must not describe an unqualified dual-transport
release as accepted.

The optional subwoofer-port fixture is only relative excitation, not absolute
acceleration or flat-response calibration. Use supported low frequencies/low level,
secure an insulated board at/outside the port mouth, strain-relieve its cable,
and stop for motion, unintended contact, chuffing/distortion, heat/smell or resets.
Record-only USB capture does not need a second output endpoint. Never feed live
microphone monitoring back into the excitation subwoofer. Direct-contact examples
are still required.
