# CM-003 deterministic acquisition

## Acceptance boundary

CM-003 supplies deterministic single-axis acquisition and a bounded source queue.
It does not connect that source to BLE; CM-006 owns the adapter between this
component and `cm_source`. No simulator result here is a claim about real-board
7174.4 Hz serviceability. Physical timing and contact response remain CM-007.

## Source timing and loss semantics

The ESP-IDF adapter configures the accepted CM-002 driver with FIFO bypass and
DRDY enabled on INT2 / GPIO13. The GPIO ISR performs only one task-notification
increment and an optional scheduler wake. It performs no I2C, allocation, BLE, or
queue manipulation.

The high-priority acquisition task drains the notification count. One notification
means one source interval. If several DRDY notifications accumulated while the
task was busy, only the newest data register value can be read truthfully. The
core therefore:

1. adds all accumulated edges to `drdy_edges`;
2. records `edges - 1` inferred missed source intervals;
3. advances the session-local u32 source index across every interval;
4. performs exactly one selected-axis read;
5. records a further missing interval when that read fails;
6. never inserts held or duplicated samples.

An I2C failure increments both the explicit I2C counter and source-miss accounting.
A non-I2C source-state fault is tracked separately. A successful read that cannot
enter the full ring increments only `ring_overruns`: the sensor interval existed,
but transport-visible data was lost later in the pipeline.

The source index intentionally wraps modulo 2^32, matching protocol-v1 arithmetic.
Lifetime diagnostics saturate at UINT64_MAX instead of wrapping.

## Ring and concurrency

The pure core contains a 256-entry power-of-two SPSC ring. The acquisition task is
the only producer. A later transport consumer may `peek` and `consume`
concurrently. The producer never overwrites unread entries; pressure drops the new
sample and preserves the existing stable prefix. Head/tail publication uses
acquire/release atomics.

Counters use a single-writer sequence snapshot so a consumer does not observe torn
64-bit values on the 32-bit MCU. Start/stop reset the ring and session-local sample
index only while producer and consumer are quiesced. Lifetime diagnostics are not
silently reset by a new session.

## Lifecycle

Board probe/reset remain the board owner's responsibility. After those succeed:

1. `cm_acq_idf_create` installs the GPIO13 ISR and high-priority task.
2. `cm_acq_idf_configure` selects axis/range/policy and calls the CM-002
   `cmq_configure` path, which enforces FIFO bypass and 6DOF register policy.
3. `cm_acq_idf_start` calls `cmq_start`. The CM-002 driver completes its
   documented settling delay before the adapter clears stale notifications,
   resets session-local queue/index state, starts the rate window, and enables the
   MCU interrupt.
4. `cm_acq_idf_stop` disables the MCU interrupt before synchronizing with any
   in-flight read, clears the queue, and stops the QMI through CM-002.
5. A later start begins a fresh source-index origin with no stale queued samples.

All sensor I/O is serialized by the acquisition adapter. The ISR never touches the
QMI. Acquisition never waits for or calls BLE.

## Rate measurement

After CM-002's sensor settling interval, each start opens a 250 ms measurement
window. Serviced notification counts include accumulated DRDY edges, so batching
does not make the edge total look artificially lower. Rate is reported in milli-Hz:

`edge_delta * 1,000,000,000 / elapsed_microseconds`.

The 250 ms window is an implementation measurement interval, not a physical
acceptance threshold or a promise that a requested ODR was achieved. A completed
zero-edge window reports zero. CM-006 must treat preparation as incomplete until
the window finishes and must reject unusable zero-rate results rather than
inventing a nominal 8 kHz rate.

## Rate policy

Fixed `safe`, `medium`, and `high` select their corresponding CM-002 modes.
`auto` accepts an explicit maximum mode from the future transport-capacity
integration. It starts at that ceiling. Within a session/policy run, an explicit
degrade request steps high -> medium -> safe, and a newly lower ceiling clamps the
mode immediately. It does not silently upshift again.

CM-003 deliberately contains no loss-percentage threshold and does not infer a
fallback decision from simulator timing. CM-006 supplies transport/health policy;
CM-007 supplies physical qualification evidence.

## Verification

`tests/native/acquisition.c` compiles the production core directly. It covers:

- FIFO ordering and full-ring preservation;
- producer-overrun accounting;
- accumulated-DRDY source-index discontinuities;
- I2C and non-I2C read failures;
- stop/restart stale-data clearing;
- u32 source-index wrap and u64 counter saturation;
- deterministic rate windows;
- high -> medium -> safe AUTO transitions and transport ceilings;
- 100,000 service/consume iterations without queue arithmetic drift.

The pinned ESP-IDF firmware build additionally compiles the real GPIO/FreeRTOS/QMI
adapter. Ordinary CI requires no board, Bluetooth adapter, or audio device.
