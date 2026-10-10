# Implementation programme

Plan reviewed and revised: 2026-10-10.

## GitHub issue map

- CM-001 -> #1
- CM-002 -> #2
- CM-003 -> #3
- CM-004 -> #4
- CM-005 -> #5
- CM-006 -> #6
- CM-007 -> #7
- CM-008 -> #8

## Plan review

The naive implementation would sample a nominal "8 kHz" accelerometer axis and immediately notify PCM over BLE. Review against the board schematic, QMI8658C Rev-A datasheet and ESP32-S3 Bluetooth capabilities exposed four failure modes:

- the actual highest 6DOF ODR is 7174.4 Hz, not a guaranteed literal 8000 Hz;
- that mode requires gyro enabled, while the board only provides 400 kHz I2C;
- QMI FIFO at high rate would include both accel and gyro and therefore exceeds the I2C byte budget;
- raw audio at high rate depends on a substantially enlarged ATT MTU; default 23-byte ATT MTU is not a credible transport target.

The revised programme isolates acquisition from BLE, uses direct single-axis DRDY reads, makes loss observable, supports measured rate fallback, fixes a versioned transport contract up front, and makes firmware/core/host behavior testable without hardware before physical qualification.

## Dependency graph

~~~text
                    CM-001 Foundation / CI
                      /        |        \
                     v         v         v
              CM-002 QMI   CM-004 BLE   CM-005 Host
                   |            |           |
                   v            |           |
              CM-003 Acquire    |           |
                    \           |          /
                     \          |         /
                      +--------> CM-006 <+
                                  |
                                  v
                             CM-007 Hardware
                                  |
                                  v
                             CM-008 Release
~~~

CM-005 may proceed after CM-001 because the wire contract is already authoritative in RAG. CM-004 may use a fake source until CM-003 is ready, but CM-006 is where real acquisition and BLE are accepted together.

## CM-001 — Repository foundation, pinned toolchain and CI

Goal: establish the reproducible project skeleton without implementing the microphone.

Deliver:

- ESP-IDF v5.5.5 project targeting ESP32-S3;
- component/layout boundaries for board driver, acquisition, protocol/transport and app;
- Python host package/test skeleton;
- native/core test harness where practical;
- GitHub Actions for firmware build + host/native tests;
- no routine large artifacts or unnecessary persistent caches;
- contributor commands documented.

Acceptance: clean build/test from a fresh checkout; CI green.

## CM-002 — QMI8658C board driver and source-mode configuration

Depends: CM-001.

Deliver:

- Waveshare board pin definitions from hardware contract;
- QMI8658C probe/WHO_AM_I/revision;
- reset and typed register driver;
- rate/range configuration including high/medium/safe candidates;
- selected-axis two-byte read API;
- DRDY/interrupt configuration primitives;
- error counters and fake bus tests;
- no BLE.

Acceptance: firmware build + native tests; register configuration evidence matches datasheet.

## CM-003 — Deterministic acquisition, ring and rate policy

Depends: CM-002.

Deliver:

- INT2 ISR wake path;
- high-priority acquisition task;
- bounded nonblocking sample ring;
- source sample index;
- missed-DRDY/I2C/ring counters;
- actual DRDY-rate measurement;
- policy state machine for high/medium/safe/auto;
- deterministic fake-source tests;
- no BLE calls in acquisition path.

Acceptance: stress/fault tests prove ordering, boundedness and observable loss.

## CM-004 — Versioned BLE GATT protocol and transport

Depends: CM-001; integrates with CM-003 in CM-006.

Deliver:

- ESP-NimBLE service/characteristics from protocol contract;
- v1 audio encoder;
- status/control;
- variable packet sizing from effective MTU;
- request large MTU/data length/2M PHY where supported;
- connection/reconnect/session lifecycle;
- nonblocking queue consumer and congestion diagnostics;
- golden protocol vectors and malformed-control tests;
- fake sample source for CI.

Acceptance: firmware builds; protocol/native tests green; acquisition API remains transport-independent.

## CM-005 — PC BLE receiver and 48 kHz audio bridge

Depends: CM-001 and accepted v1 RAG protocol.

Deliver:

- Python package/CLI;
- Bleak discovery/connect/control/notify;
- v1 decoder and session handling;
- exact sample-index gap accounting;
- bounded loss concealment;
- DC blocker/high-pass, gain and limiter;
- stateful resample to 48 kHz mono;
- sounddevice live sink and WAV recorder;
- diagnostics and virtual-audio-cable documentation;
- no-hardware unit tests.

Acceptance: golden vectors decode identically; deterministic DSP/resampler/buffer tests green.

## CM-006 — End-to-end integration, simulation and adaptive quality policy

Depends: CM-003, CM-004, CM-005.

Deliver:

- real acquisition wired into BLE packetizer;
- configuration changes safely stop/reconfigure/restart sessions;
- auto rate policy incorporates acquisition/transport capacity without hiding loss;
- deterministic simulator drives end-to-end frames into host receiver;
- fault injection for sensor misses, I2C faults, queue overrun, BLE congestion, packet loss and reconnect;
- bounded latency/buffer behavior;
- consolidated diagnostics.

Acceptance: automated end-to-end qualification green; no hardware performance claims yet.

## CM-007 — Real-board hardware qualification and default tuning

Depends: CM-006.

Requires: physical Waveshare ESP32-S3-Matrix and a BLE-capable host.

Deliver:

- flash/qualification scripts and reproducible procedure;
- measured high and medium source rates;
- 10-minute sustained runs using metrics from verification contract;
- axis/range comparison;
- MTU/transport evidence;
- contact-vibration recordings/evidence as appropriate;
- LED-off baseline and any status-display interference check;
- choose production `auto` default from evidence;
- fix only issues required to meet qualification, with tests.

Acceptance: physical evidence satisfies `docs/rag/06-VERIFICATION-AND-CI.md`; if high mode misses target, medium becomes accepted default rather than concealing loss.

## CM-008 — Release hardening and acceptance reconciliation

Depends: CM-007.

Deliver:

- user build/flash guide;
- host installation/usage guide;
- troubleshooting for BLE MTU, missing adapter, virtual audio cable, clipping and fallback;
- architecture/protocol docs reconciled with implemented behavior;
- clean fresh-checkout build/test;
- version/about reporting;
- final accepted hardware defaults documented;
- no stale TODO claiming unqualified 8 kHz behavior.

Acceptance: complete automated CI green, documented hardware evidence linked, default branch post-merge verification green.

## Common issue completion rule

Every GitHub issue created from this programme is an executable agent prompt.

The agent must read `AGENTS.md`, `RAG.md`, issue-specific documents and all issue comments; reconcile live state; implement; test; open a PR; monitor CI at **no less than 120-second intervals**; fix failures; merge only after all required automated checks are green; then verify the default branch and record acceptance.

Hardware issue CM-007 must not be merged as "qualified" without actual physical evidence.
