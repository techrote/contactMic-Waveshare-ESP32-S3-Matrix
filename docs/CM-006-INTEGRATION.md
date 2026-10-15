# CM-006: integrated source, session lifecycle and fault qualification

## Current software boundary

The app now opens the accepted 400 kHz board bus, probes/resets the QMI8658C and
attaches real acquisition to the existing custom GATT service. START prepares the
sensor, measures its source rate and returns PREPARED; COMMIT_START opens a fresh
indexed stream. Initialization failure leaves an explicitly unavailable source,
not synthetic audio. No physical hardware was used to qualify this milestone.

This document supersedes the **source-unavailable/current-app** descriptions in
CM-003/004/005 milestone notes. Their driver, wire and host contracts otherwise
remain intact. In particular, all v1 bytes, UUIDs, canonical vectors and the
one-committed-stream-per-connection rule are unchanged. Real reset behavior,
DRDY/I2C timing feasibility, RF throughput, audio quality and production defaults
remain CM-007. LEDs are not driven here; an LED-off physical baseline must be
established and observed during that qualification.

## Task ownership and nonblocking handoff

| Owner | Work | Work excluded |
| --- | --- | --- |
| GPIO13 ISR | Bounded edge recording and task wake | I2C, BLE, heap allocation, sample queue |
| Acquisition task | Selected-axis read, indexed ring producer, calibration draining/rate measurement | BLE calls, frontend waits |
| Controller task | Configure/settle/stop, preparation state machine, deferred diagnostic logging | BLE calls, concurrent ring consumption while visible |
| NimBLE owner | GATT controls, publication gate, packetization, status and adaptive policy | Sensor I/O, lifecycle mutex waits, blocking counter snapshots or logging in the observer |
| Host worker | Existing decoder, loss accounting, DSP/resampler and sink | Hardware access during automated tests |

`contactmic_session` joins existing production acquisition and transport through
`cm_source`. It does not copy their algorithms. Frontend-to-controller commands
and controller replies have atomic 32-bit fields, a sequence stamp and a generation.
Readers try once; contention returns BUSY or preserves the last coherent snapshot.
Generation exhaustion rejects a new session instead of wrapping into stale reuse.

A PREPARE/COMMIT reply is usable only for the currently requested generation.
STOP immediately makes all old samples logically inaccessible to the only external
consumer, then posts asynchronous shutdown. The controller subsequently clears
physical storage under acquisition ownership. A blocked old begin, late READY,
late producer acknowledgment or rapid stop/reconnect cannot reopen publication.
Repeated STOP does not create an unbounded queue or repeated retry backlog.
Preparation and publication acknowledgment have bounded two-second worker budgets;
backend failures stop once before reporting failure. An explicit later request
may attempt recovery; there is no blind retry loop hiding errors.

The new application context has fixed boot lifetime. It does not destroy a live
acquisition task or free its context during connection churn. Initialization
failure retains that bounded context and reports source unavailable. This does
not qualify the legacy standalone destroy API for arbitrary cross-core callers.
Lifecycle operations must have one controller owner; ring reset requires external
publication to be gated. The controller is the only caller of the blocking IDF
source lifecycle API. Its delays never execute on the NimBLE host task.

## Calibration, index origin and measurement

After the CM-002 settling delay, preparation runs acquisition with external
publication closed. The acquisition task consumes calibration samples itself, so
waiting for a host commit cannot fill the ring. Lifetime diagnostic observations
are retained, but calibration samples never become stream audio.

COMMIT posts a publication request. The acquisition task clears the calibration
prefix and rebases only the session-local sample index to zero, preserving the
measured rate and lifetime counters. Its acknowledgment precedes the controller's
LIVE reply, which precedes any external ring access. This creates the index origin
without a blocking GATT callback or metadata changes midstream.

Rate is derived from observed ISR edge totals paired with elapsed microseconds,
including edges still pending during a slow I2C operation. It is not inferred from
successful-read throughput or substituted with a nominal rate. A zero or
unrepresentable measured rate fails preparation. Checked integer arithmetic avoids
the pre-existing `edges * 1e9 + elapsed/2` rounding overflow. The regression test
fails on the accepted pre-CM-006 source and passes on the corrected production code.

The selected-axis adapter checks GPIO13's DRDY-high window and pending edges before
and after its two-byte transaction. A low window or observed intervening update
rejects the sample and preserves a source-index gap; successful I2C alone does not
prove a coherent sensor value. These are conservative software checks. They do
not prove that every physical edge is observed, eliminate interrupt-latency races,
or establish a feasible high-rate read window. CM-007 must measure those limits.

## Adaptive policy and observable boundaries

The unqualified bootstrap ceiling is selected by
`CONFIG_CONTACTMIC_AUTO_CEILING` (0=safe, 1=medium, 2=high), default **safe**. This is
an explicitly conservative pre-hardware construction choice, not a hardware-selected
production default. Fixed safe/medium/high requests remain available for candidate
tests. AUTO never silently raises its boot-lifetime ceiling after a fallback.

The transport-capacity bound is the packetizer's samples per notification times
its maximum scheduled notifications per second. The latter is derived from the
actual NPL tick rounding and the existing four-packets-per-pump budget. It is only
an upper bound: neither a PHY request nor a successful stack submission establishes
RF delivery. Both nominal candidate screening and the actual measured rate must
fit. Impossible fixed/safe requests return CAPACITY; AUTO may choose a lower mode.
For example, MTU 23 with a 400-notification/s scheduling budget cannot carry even
the safe candidate. The implementation refuses rather than concealing gaps.

While streaming, AUTO observes one-second diagnostic windows. Any new source miss,
I2C failure, ring overrun or notification-failure observation requests a conservative
step down. This is a deterministic safety policy, **not** a physical loss threshold
inferred from simulator performance. The CM-007 high/medium acceptance percentages
are unchanged. A newly insufficient scheduling capacity or failed backend also
closes the current stream. Fixed policies do not silently change because of a
health counter; capacity/backend failure still closes an unusable stream.

A fallback issues STOP, reports zero active rate and the next selected mode in
full status, and records a reason/transition event for controller-side serial
logging. The existing host observes STOPPED on its periodic full status read and
ends the run with a reconnect-required error; cleanup retrieves final diagnostics.
**This is not seamless automatic reconnection.** Start a new host run/connection to
prepare the selected lower AUTO mode. A committed connection cannot be reused and
an old WAV is not silently spliced across sessions. A future seamless host policy
would need explicit sink/session handling, not an in-place v1 metadata mutation.

Firmware lifetime loss counters survive fallback, stop, reconfiguration and
reconnect. Only the existing explicit stopped-state diagnostic-reset control
changes reported baselines. Source misses, notification rejection and host gaps
can describe overlapping lost material; do not sum them as independent RF loss.
Calibration observations remain lifetime diagnostics even though the committed
stream has a fresh index origin. Ring overflow preserves the readable prefix and
drops new samples visibly; host ingress/output pressure retains its existing
bounded drop-oldest behavior. Queue capacities bound backlog, not physical latency.

## Reproducible qualification

From a fresh repository checkout, with Python 3.11+ and GCC/Clang/CMake available:

```sh
cmake -S tests/native -B build/native -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native --parallel 2
ctest --test-dir build/native --output-on-failure --no-tests=error
python -m venv build/host-verify
build/host-verify/bin/python -m pip install ./host
build/host-verify/bin/python -I tests/integration/test_cm006.py --simulator build/native/cm006_simulator
```

Use the virtual environment's `Scripts/python.exe` path on Windows. The native
reference qualification is Linux/GCC or Clang; the existing complete host test
matrix remains Linux/Windows, Python 3.11/3.14. Configure `tests/native/cm006`
instead of `tests/native` for just the new native apparatus. No source-path injection
or real Bluetooth/audio device is required. Firmware CI still uses ESP-IDF v5.5.5.

The simulator emits a bounded JSON-lines trace generated by the real C session,
acquisition, transport and codecs. The Python test runs it twice and requires
byte-identical output, then uses the installed production decoder/DSP/FIFO/WAV
classes. It adds host-side packet faults explicitly. Synthetic clock/rate values
are deliberately distinct from nominal hardware ODRs. Boundary seeds near u32/u16
wrap are explicit internal test injections, not normal stream-start history.

| Fault or property | Exercised production path |
| --- | --- |
| Accumulated DRDY, I2C failure, ring overflow | Core -> ring -> packetizer -> Python gap/flag/counter checks |
| Notify rejection and host packet loss | Real attempted sequence/index progression -> host accounting |
| Duplicate/reordered input | Production host tracker rejects without corrupting DSP history |
| Stop during pending preparation, stale completion | Generation mailbox and publication gate |
| Reconnect, axis/range/rate change | Fresh handshake/index origin, no stale queued prefix |
| Low MTU and high -> medium -> safe | Scheduling-capacity refusal and retained AUTO ceiling/counters |
| Source-index and packet-sequence wrap | Production C framing and bounded host modular gap handling |
| Audio sink slowdown | Production 128-sample FIFO retains newest output; explicit overrun/underrun |
| Packet partitioning | Identical float32 resampling output across variable BLE packet sizes |
| Calibration and DRDY read-window ordering | Real IDF acquisition C compiled against named GPIO/QMI/scheduler fakes |
| Counter contention and integer overflow | Bounded atomic snapshot and minimized pre-fix rate regression |
| Concurrent producer/consumer | One million real SPSC operations across ring-index wrap; all drops accounted |

The injected fault chain intentionally produces **162 host-visible missing source
intervals**: 3 DRDY misses, 1 I2C failure, 114 rejected-notification samples and
44 ring-overrun samples. Firmware reports `(source_misses=4, i2c_errors=1,
ring_overruns=44, notify_rejections=1, packets_sent=6)` separately. These are exact
synthetic test expectations, not measured hardware error rates.

No large output recordings, build trees or persistent caches are committed. All
new tests are added to normal CI; no duplicate benchmark/qualification workflow
is dispatched. Actual build/job identities and acceptance are recorded on issue #6.
