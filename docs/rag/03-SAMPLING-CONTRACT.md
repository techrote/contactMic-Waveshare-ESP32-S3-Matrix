# Sampling contract

## Source modes

The firmware exposes source-rate policies rather than promising an exact literal rate.

Logical policies:

- `high`: request QMI8658C 7174.4 Hz 6DOF mode;
- `medium`: request 3587.2 Hz 6DOF mode;
- `safe`: request approximately 896.8 Hz 6DOF mode or an evidence-backed equivalent;
- `auto`: start with the highest qualified mode and fall back when qualification/transport policy requires it.

The actual measured source rate in milli-Hz is authoritative in stream metadata.

The implementation may refine the safe mode during hardware qualification, but protocol meaning must remain stable.

## Acquisition path

Preferred high/medium path:

1. QMI8658C FIFO bypassed;
2. DRDY routed to INT2 / GPIO13;
3. ISR records/wakes only; it must not perform I2C transactions or BLE work;
4. a high-priority acquisition task reads exactly the selected accelerometer axis;
5. sample and monotonic source-sample index are pushed into a bounded queue/ring;
6. BLE consumes from that queue independently.

No BLE API may run in the acquisition critical path.

## Timing and loss accounting

Maintain at least:

- DRDY edge count;
- successful axis-read count;
- inferred missed DRDY count;
- I2C error count;
- sample-ring overrun count;
- active requested ODR;
- measured source rate.

Do not silently duplicate/hold samples in firmware to disguise a missed sensor interval.

If a source interval is missing, preserve the discontinuity in sample-index/telemetry so the host can decide how to conceal it.

## Rate measurement

At startup and whenever source mode changes, measure actual DRDY rate over a bounded interval after sensor settling.

Publish the measured rate as a session/status property. Audio packet timestamps/sample-index arithmetic uses the measured/declared session rate, not a hard-coded 8000 Hz value.

## High-rate qualification policy

The 7174.4 Hz mode is a target subject to real-board measurement.

A hardware qualification run must measure missed intervals and I2C errors over a sustained interval.

If high mode cannot meet the accepted loss threshold, the production `auto` policy must choose `medium` rather than hiding loss.

No implementation issue may lower acceptance thresholds merely to retain the higher advertised rate.

## Queueing

Use a bounded power-of-two ring or equivalent bounded structure.

Requirements:

- producer cannot block on BLE;
- overflow increments an observable counter;
- packetizer can retrieve ordered samples plus their first sample index;
- disconnect/reconnect creates a clear new stream/session boundary;
- stale queued audio must not leak into a new session.

## Sample representation

Firmware transport uses signed 16-bit little-endian accelerometer samples in the sensor's native signed representation unless a later protocol-major change says otherwise.

Gravity/DC removal, gain and resampling belong on the host so raw mechanical information and clipping behavior remain inspectable.

## Testability

Acquisition scheduling, counters, ring behavior and rate-policy state transitions must be testable with a fake sample source on a normal CI host. Physical I2C/ISR behavior is separately hardware-qualified.
