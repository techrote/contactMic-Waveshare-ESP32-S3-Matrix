# CM-009: acquisition feasibility and qualification-contract addendum

## Decision and authority

**Software research/audit candidate; MEDIUM acquisition blocked; USB and BLE physical acceptance held.** This is the fallback disposition authorized by issue #16's cloud investigation, not a claim to have implemented a viable MEDIUM acquisition correction.

The pre-implementation proposal and full eight-part analysis are preserved on [#16, comment 6215982885](https://github.com/techrote/contactMic-Waveshare-ESP32-S3-Matrix/issues/16#issuecomment-6215982885). The examined parent is `15d97bb2adb10c7b152d2cfd9bd1ca6f2b06b4e9`, based on current main `0d4076131f9ff699b44a1eb3bcf8fcf95f99c93d`. Continue the existing branch and draft #17.

This document is an explicit **qualification addendum** to `CM-009-USB-AUDIO-CONTRACT.md` and the source index `rag/00-SOURCES.md`. Subsequent capture review must run both the existing capture evaluation and the new offline acquisition audit described below. It does not replace historical evidence or amend the sensor behavior accepted by CM-002, CM-003, CM-006, `rag/02-HARDWARE-CONTRACT.md` or `rag/03-SAMPLING-CONTRACT.md`.

The contemplated change was replacing the whole-transaction DRDY-high/no-intervening-edge rule with a vendor-supported shadow-lock guarantee. **That acquisition amendment is not adopted:** the documented mechanism cannot sustain MEDIUM at 400 kHz. The USB-only CTRL1=0x50 output-gate amendment remains exactly as accepted; it is not a coherency waiver. No speculative locking, FIFO, preaddressed read, timestamp-based denominator or unchecked read is implemented.

## Primary sources and evidentiary weight

QST **QMI8658C**, document **13-52-27, Rev A**, revision-history date **20 June 2022**: [manufacturer PDF](https://www.qstcorp.com/upload/pdf/202210/13-52-27%20QMI8658C%20Datasheet%20Rev%20A%20(1).pdf). Relevant sections: 5.3/Table 22; 5.5; 5.10.1, 5.10.4–5, 5.10.6.3 and 5.10.6.13; 6.1–3; 8.1–10; 13.1–5; 16.1 and 16.3/Table 43. Text was inspected directly. The web PDF renderer failed; waveform figures were **not independently visually verified**. No C-revision erratum resolving the remaining question was found in the inspected primary sources.

Board-vendor source: `waveshareteam/ESP32-S3-Touch-AMOLED-2.41-V2` at **`0eaf16cfae3440d445f2b6caa71e94065094551f`**:

- [Arduino header](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.41-V2/blob/0eaf16cfae3440d445f2b6caa71e94065094551f/02_Example/Arduino/03_I2C_QMI8658/qmi8658c.h).
- [ESP-IDF driver example](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.41-V2/blob/0eaf16cfae3440d445f2b6caa71e94065094551f/02_Example/ESP-IDF/10_FactoryProgram/components/qmi8658c/qmi8658c.c).

The header assigns CTRL1 bit 4 to INT2 enable whereas QST Table 22 marks bits 4:3 reserved. That is the existing output-gate contradiction, not evidence of a different data latch. The example conditionally compiles SyncSample, can return previous cached values when data are unavailable, does not consistently propagate bus failures, and implements its microsecond-named delay using `vTaskDelay`. Its enum names and working examples cannot establish the required source rate, per-conversion delivery or timing. The code is evidence of vendor practice, not a performance/coherency guarantee beyond the manufacturer's documented procedure.

## Complete mechanism and loss consequences

The normative sequences and unresolved details are itemized in the linked issue proposal. In brief, QST section 13 requires AHB clock-gating disable for I2C at all ODRs, the complete CTRL9 handshake, SyncSample enable, STATUSINT availability/lock handling and a terminal-byte read to release the shadow lock. The 6DOF terminal byte is **GZ_H**, not the selected accelerometer MSB. New results arriving while locked are dropped. MEDIUM requires the stated 2 us lock delay when locking is not already complete. Setup/exit and ODR changes also have command/settling/flush obligations; a single register write is not a complete transaction protocol.

FIFO read mode is also not a concurrent-fill buffer. Sections 8.7–9 require entry handshake, correct word-count/drain handling and exit; incoming data during read mode are discarded. The existing 6DOF source produces 12-byte FIFO records, not two-byte selected-axis records. Overflow flags do not provide an exact lost-conversion count.

STATUS0 availability is not an overrun counter. A 24-bit timestamp description does not by itself establish what it counts under shadow locking, read-mode discards, reset and recovery. No inspected guarantee permits retaining an I2C register pointer across STOP and using an independently latched/current-address transfer to cross the pulse safely. Section 16's burst auto-increment is insufficient proof of those semantics. Accel-only operation cannot retain this MEDIUM 6DOF-clocked source policy by simply keeping a similarly named enum value.

## Reproducible lower bounds

For a repeated-start register read of N bytes, `9 * (3 + N)` SCL periods account for address-W, register address, address-R, data, and ACK/NACK. These are optimistic wire floors, excluding START/STOP/setup/bus-free time, stretching, all driver/RTOS/ISR work and scheduling.

| Operation | SCL periods | Minimum at 400 kHz |
| --- | ---: | ---: |
| Selected signed 16-bit axis | 45 | 112.5 us |
| Hypothetical sparse lock: status + axis + GZ_H | 117 | 292.5 us |
| Status + Z through GZ_H burst | 135 | 337.5 us |
| Status + Y through GZ_H burst | 153 | 382.5 us |
| Status + X through GZ_H burst | 171 | 427.5 us |
| One 6DOF FIFO record | 135 | 337.5 us |
| 128 6DOF FIFO records | 13851 | 34627.5 us |

The sparse sequence deliberately grants a shortcut not established by the documented terminal-burst procedure. **Even this optimistic bound fails:** reported 3569.987 Hz MEDIUM has a 280.113 us interval, while 117 clocks permit at most 3418.803 reads/s. Over a long free-running interval at least **4.23485%** of updates go unserved before software overhead, versus the unchanged **0.01%** limit. The full documented path is slower still. A lock cannot improve this by pausing or hiding conversions.

A 128-record FIFO drain spans at least 123 additional source updates even under favorable phase. A test that pretends FIFO filling continues during that read would promise behavior the vendor does not. The raw 12-byte payload alone uses 96.389649% of the bus at the reported MEDIUM rate; that utilization number is not evidence of feasibility.

Reproduce the arithmetic without devices:

```sh
python -I tools/cm009_acquisition_audit.py budget --source-rate-millihz 3569987 --i2c-hz 400000
```

The reported DRDY high windows and 225 us read-call-plus-post-check minimum are software observations from the prior operator report, not new electrical measurements. The selected-axis wire floor itself exceeds the reported MEDIUM 100–106 us high window. The current IDF adapter already retains device handles; it does not create/delete an I2C device for every sample. Scheduling, timer/atomic/pin checks and I2C driver overhead may worsen timing, but removing them cannot defeat a wire lower bound. Diagnostic polling is preparation-only and holds acquisition ownership while IRQs remain enabled; it is neither sample acquisition nor part of the evaluated duration.

## Qualification amendment: actual source ledger

The additive installed module `contactmic.usb_acquisition_audit` calls the unchanged capture evaluator and preserves its verdict in `capture_evaluation`. It independently checks the source ledger and exposes actual interval and successful-read throughput. It never opens devices, rewrites manifests, inserts samples or changes firmware.

For the current production core, a fresh snapshot has `intervals = reads + missed + pending`, where pending is **0 or 1**. The interval is recorded before the synchronous read completes, outside the counter seqlock. Therefore a matched-window partition residual can be -1, 0 or +1. Treating every fresh snapshot as fully completed would falsely reject valid evidence. A new C regression observes the actual core *inside* its read callback to establish this boundary.

Ring overruns are successful reads discarded by the bounded ring; they are separate from acquisition misses and must not be double-counted. Read-failure subsets, counter monotonicity, saturation and stale snapshots are checked. Full identity/generation/configuration/USB/host/four-clock checks remain the original evaluator's responsibility and are always run by the wrapper. Nominal rates and USB output frames never replace the actual source denominator.

The audit compares interval cadence against preparation rate and preparation rate against the commanded mode's nominal reference. A **3% gross-discrepancy review trigger** matches the existing servo range. It is **not** a guaranteed vendor oscillator tolerance, a 3% loss allowance, or evidence that sub-3% throttling is safe. QST's typical oscillator precision does not establish a guaranteed bound. A consistent fabricated or paused clock could still satisfy these internal checks: independent evidence of free-running source behavior remains mandatory for any new acquisition mechanism.

Fixed MEDIUM preflight also fails this supplement when actual misses already exceed 0.01% or I2C errors are nonzero. The existing capture command is unchanged; this supplement is offline and does not claim to add a live source-loss abort. Do not advance from a completed preflight merely because its original summary says `evidence_complete`.

## Tests, limitations and next decision

Tests cover wire floors; independent conversion-clock capacity; torn reads versus locked/drop semantics; stalled lock/reset exclusion in an explicitly synthetic reference; source ledger boundaries, retained large counters, saturation and regressions; immutability and full build identity checks. Integration regressions demonstrate the old evaluator accepting fabricated half-rate or impossible ledgers, then the supplement holding them. The reference model deliberately does not invent sensor interrupt/timestamp visibility for dropped updates. No lock driver or hardware timing has been simulated into acceptance.

Normal CI retains both IDF v5.5.5 firmware builds, all native/CM-006/tooling tests and Linux/Windows Python 3.11/3.14 host tests. Host jobs now use non-editable venv installs, isolated imports and source-byte/provenance verification. Final exact-head results belong in the issue/PR checkpoint; pending CI is not a pass.

**Needed vendor answer for C revision 0x7C:** provide a documented <=400 kHz I2C sequence retaining every approximately 3.57 kHz 6DOF-clocked selected-axis update, with explicit latch/update timing and exact discarded-conversion accounting. Confirm or reject STOP-retained/preaddressed/current-address semantics, or supply an applicable erratum or genuinely concurrent-buffered mechanism. Without that evidence, a different interface/hardware/rate is a separately authorized architectural decision, not an implicit relaxation here.

No new sustained physical run is justified on the unchanged acquisition. See `CM-009-CLOUD-HANDOFF.md`. Keep #17 draft and #16 open, #7/#15 and BLE hardware held; do not enter CM-008. Roll back any future workaround that requires overclocking, unknown register semantics, hidden pauses/loss, stale replay or a changed denominator.
