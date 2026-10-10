# Verification and CI contract

## Toolchain baseline

Initial firmware toolchain: ESP-IDF v5.5.5 targeting ESP32-S3.

A later accepted issue may update the pin, but every PR must use the repository-pinned version.

CI should avoid unnecessary stored artifacts/caches.

## Required automated layers

Every PR must run the layers relevant to its changes; by system-integration time the repository must have all of these:

### Firmware build

- configure target ESP32-S3;
- build cleanly under pinned ESP-IDF;
- warnings introduced by project code are treated seriously and must not be blindly suppressed.

### Native/core tests

Run host-native tests for pure firmware logic where practical:

- ring/queue;
- sample index accounting;
- packet encoder;
- rate-policy state machine;
- control parsing;
- backpressure/error transitions.

### Host Python tests

At minimum:

- protocol vectors;
- malformed frames;
- gap detection;
- DSP/DC blocker;
- resampler continuity across packet boundaries;
- bounded FIFO behavior;
- WAV writer;
- CLI import/smoke tests.

Run on Linux; add Windows CI once host audio/packaging behavior warrants it. Tests must not require a real BLE adapter/audio device.

### End-to-end simulation

A deterministic simulated source feeds firmware-side packetization/reference vectors into host decoding/DSP.

Fault injection must cover:

- missed source intervals;
- packet loss;
- packet reordering/duplicates where applicable;
- queue overflow;
- BLE congestion/rejected send;
- disconnect/reconnect;
- sample-index/sequence wrap boundaries;
- malformed control writes.

## CI polling discipline

Agents monitoring GitHub Actions must obey \`AGENTS.md\`: at least 120 seconds between status queries while checks remain pending/running.

Do not compensate for slow jobs with tight polling.

## Hardware-in-loop qualification

Hardware issue CM-007 is the first issue allowed to make real-board performance claims.

Required evidence on a stock Waveshare ESP32-S3-Matrix:

- detected QMI8658C address, WHO_AM_I and revision;
- actual DRDY rate for each tested source mode;
- I2C error count;
- missed DRDY/source-interval count;
- ring overrun count;
- BLE effective MTU and, where observable, PHY/data length;
- host source-gap/packet-loss count;
- host audio underrun count;
- sustained run duration;
- axis/range used;
- LED state during the test.

### High-rate acceptance target

For an initial sustained 10-minute connected run at the high source policy:

- no firmware crashes/resets;
- no unbounded queue growth;
- I2C errors: 0 preferred;
- missed source intervals <= 0.1% target;
- no sustained host audio underrun.

If high mode exceeds the loss target after reasonable implementation tuning, that is evidence to make \`auto\` default to medium. It is not permission to conceal loss.

### Medium fallback target

For a sustained 10-minute connected run at medium:

- no firmware crashes/resets;
- no unbounded queue growth;
- I2C errors: 0;
- missed source intervals <= 0.01%;
- no sustained host audio underrun.

Thresholds may only be changed with documented evidence and a contract update.

## Contact-audio qualification

Capture at least short reproducible examples for:

- finger/tap impulse on a rigid surface;
- scraping/rubbing;
- a resonant object or panel.

Compare X/Y/Z and +/-2 g vs +/-4 g enough to choose the production default. Record clipping and noise observations; do not claim subjective fidelity as a numeric metric.

## Merge policy

Required automated checks must be green before merge.

Hardware-required checks may be separate from ordinary PR CI, but CM-007 itself cannot be accepted without physical evidence.

After merge, agents follow the post-merge verification protocol in \`AGENTS.md\`.
