# CM-004: wire v1 and transport implementation

This document completes fields left unspecified by
[the v1 RAG contract](rag/04-BLE-PROTOCOL-CONTRACT.md). It does not change its
UUIDs, audio offsets, sample representation, flags or reserved-zero requirements.
Before this change is accepted, these details are proposed CM-004 definitions;
after merge they are the implementation authority used by CM-005.

## Scope and present firmware behavior

The real ESP-NimBLE GATT service is compiled and started by `app_main`. The
production transport consumes a generic nonblocking source interface. **The
shipped app has no acquisition adapter yet:** start requests return UNSUPPORTED,
not synthetic audio. CM-006 integrates CM-003's measured real source. No board,
RF throughput, sustained sample rate or audio quality is qualified here.

`contactmic_protocol` owns pure codecs; `contactmic_ble` owns transport and the
IDF adapter. BLE never calls the QMI driver or waits for acquisition. Source
callbacks must be bounded/nonblocking; prepare can return BUSY while the source
owner independently configures, settles and measures. Stop synchronously gates
publication and discards the queue, without waiting for hardware shutdown.

## Audio and descriptor

The 16-byte audio header is unchanged. Descriptor bits 0–1 are axis X=0/Y=1/Z=2;
value 3 is invalid. Bit 2 selects +/-4 g; zero selects +/-2 g. Bits 3–7 are zero.
Count is 1–255. Zero source rate, unknown major, reserved flags/fields, invalid
descriptor and non-exact payload lengths are rejected. All integers are LE.

Notification capacity is `min(255, floor((MTU - 3 - 16) / 2))`. The actual adapter
caps its MTU usage at 247: MTU 23 carries two samples; MTU 247 carries 114. It
never joins samples across an internal source-index gap. Index arithmetic wraps
modulo 2^32, sequence modulo 2^16. Producer gaps are not filled or renumbered.
Sequence increments for every attempted audio notification, including rejected
attempts. A rejection consumes that bounded packet and increments diagnostics;
the next successful packet preserves the resulting index/sequence discontinuity.

`packets_sent` counts submissions accepted by the BLE stack, **not peer delivery
acknowledgements**. `notify_rejections` counts immediate rejection or later failed
completion observations; some stacks can report both for one attempted packet.
It is not a unique-packet-loss estimate. The receiver uses sequence/index gaps
and source counters rather than adding these overlapping measurements together.

## Control requests and responses

Requests are 8-byte headers followed by exactly the required payload:

| Offset | Type | Request field |
| --- | --- | --- |
| 0 | u8 | major = 1 |
| 1 | u8 | opcode |
| 2 | u16 | caller request ID |
| 4 | u16 | payload byte count |
| 6 | u16 | reserved = 0 |
| 8 | bytes | payload |

| Opcode | Operation | Payload |
| ---: | --- | --- |
| 1 | capabilities | empty |
| 2 | current configuration/status | empty |
| 3 | set axis | u8: 0/1/2 |
| 4 | set range | u8: 2/4 g |
| 5 | set policy | u8: safe=0, medium=1, high=2, auto=3 |
| 6 | start: prepare session | empty |
| 7 | stop | empty |
| 8 | diagnostic reset | empty |
| 9 | commit prepared start | nonzero u32 prepared session ID |

Responses: byte 0 major=1; byte 1 opcode OR 0x80; u16 request ID at 2;
u8 result at 4; zero at 5; u16 payload length at 6; payload at 8.
Result values: OK=0, INVALID=1, UNSUPPORTED=2, STATE=3, BUSY=4, IO=5, CAPACITY=6.

Use GATT writes **with response**, then read the control characteristic and
validate opcode/request ID before interpreting it. Serialize requests. Malformed
writes fail at ATT without changing configuration or replacing the previous
response. Valid operations that cannot be applied return their application
result in the control read. A valid non-capabilities response carries a full
64-byte status even on an application error. The response is at most 72 bytes.

Successful capabilities payload is eight bytes: protocol major=1, layout
revision=1, supported axis mask=7, supported range mask=3 (bit0=2g, bit1=4g),
policy mask=15, feature mask, maximum requested MTU as u16=247. Feature bits:
0=prepare/commit required, 1=one committed session per connection, 2=source adapter
attached; other bits zero. A source-unavailable build does not set bit 2.

## Status and long reads

Status read is 64 bytes. Status notification is a compact 20-byte event, fitting
even MTU 23. Full snapshots expose all counters; events expose session/config.

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | u8 | major=1 |
| 1 | u8 | kind: full=0, event=1 |
| 2 | u16 | exact length: 64 or 20 |
| 4 | u32 | session ID |
| 8 | u32 | source rate in milli-Hz; zero only while stopped/unavailable |
| 12 | u16 | effective ATT MTU |
| 14 | u8 | audio descriptor |
| 15 | u8 | requested policy |
| 16 | u8 | STOPPED=0, PREPARED=1, STREAMING=2 |
| 17 | u8 | selected safe=0, medium=1, high=2; meaningful after preparation |
| 18 | u8 | observed TX PHY: unknown=0, 1M=1, 2M=2, coded=3 |
| 19 | u8 | reserved=0 |
| 20 | u64 | source missed intervals |
| 28 | u64 | source I2C errors |
| 36 | u64 | source ring overruns |
| 44 | u64 | notification failure observations |
| 52 | u64 | stack-accepted audio submissions |
| 60 | u16 | observed data length, zero if unavailable |
| 62 | u16 | reserved=0 |

The final seven fields are absent from compact events. Full status/control
values use ordinary ATT long reads at low MTU. The adapter freezes a separate
read snapshot at offset zero and retains it for subsequent read-blob offsets;
NimBLE applies the offset. A timer update cannot tear a multi-fragment counter.
Only one connection is supported; clients must serialize control reads/writes.

Source counters must be monotonic lifetime counters. The transport reports their
deltas from the last explicit diagnostic reset and retains evidence over
reconnects. An unexpected regression latches a source fault and prevents further
stream starts; reconnect/reset-diagnostics cannot erase that fault. Correct the
source adapter and reinitialize its ownership lifecycle. Counters saturate, not
wrap. Diagnostic reset is accepted only while stopped, never implicitly on
fallback, reconfiguration or disconnect.

## Session ordering and stale-data exclusion

An audio packet has **no session ID**. To avoid pretending a status notification
alone can order old OS callbacks, this implementation permits **at most one
committed stream per BLE connection**. Configuration changes happen while stopped;
after a committed stream has stopped, reconnect before starting another.

Host sequence: connect; optionally configure axis/range/policy; subscribe to
status; send START and read PREPARED status; bind decoder/DSP to that session,
rate and descriptor; subscribe to audio; send COMMIT_START with that session ID.
No audio is emitted before a matching commit and audio subscription. The client
must be ready for callbacks before the commit response arrives. Source prepare
may return BUSY and may be retried with a bounded timeout; configuration writes
and diagnostic reset are blocked during preparation until STOP cancels it;
no sensor work runs
inside the GATT callback. A failed prepare cannot publish samples.

Stop, unsubscribe, stack reset and disconnect close publication and clear queued
samples. A new connection starts with subscriptions off and sequence zero on the
next prepared session. Session IDs increment within a boot and never wrap into
reuse: exhaustion requires reinitialization. A random boot prefix is diagnostic,
not the stale-data safety mechanism. The host also binds callbacks to a unique
local connection generation and rejects callbacks after that generation closes.

Runtime automatic fallback and reconfiguration integration remain CM-006 work.
They must retain these semantics (or introduce a reviewed compatible handshake),
not silently change metadata midstream. Production default selection is CM-007.

## IDF adapter and validation

The adapter is built against ESP-IDF v5.5.5 and its esp-nimble submodule
`685675c0128deafdd201c9eb82e61d227364646c`. API references:
[ESP-NimBLE](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/bluetooth/nimble/index.html),
[pinned GAP header](https://github.com/espressif/esp-nimble/blob/685675c0128deafdd201c9eb82e61d227364646c/nimble/host/include/host/ble_gap.h),
[GATT server long-read context](https://mynewt.apache.org/latest/network/ble_hs/ble_gatts.html).

Legacy advertising carries the service UUID and BLE-only flags; the scan response
carries the complete S3-ContactMic name. MTU preference is 247, DLE requests 251
bytes/2120 us, and PHY preference requests 2M. None is guaranteed. TX PHY is read
back; data length remains explicitly unknown if the adapter cannot observe it.
The transport uses effective ATT MTU, not a host-library guess. NVS errors are
reported without automatically erasing stored data. The service currently has
no pairing/authentication policy; use a trusted test environment. CM-008 must
reconcile user-facing deployment limitations rather than imply secure pairing.

All transport work runs on the serialized NimBLE host event queue. Each timer
invocation emits at most four packets, with a minimum one-tick delay even where
5 ms rounds to zero. Status refresh is at approximately one second plus explicit
state changes. Advertising retries are bounded. No extra producer queue or
unbounded retry backlog is introduced.

Golden wire bytes are plain hex in `tests/vectors/v1/`, with independent declared
field values in `manifest.json`. Native tests encode the production structs and
compare exact bytes. CM-005 consumes the same files. Tests also cover invalid
lengths/reserved fields/versions, min/max MTUs, control validation before effects,
sequence/index wrap, rejection, source gaps, source-counter regression,
subscription/session transitions and 100000 bounded fake-source iterations.
Firmware CI compiles the actual NimBLE adapter; native tests are not physical
radio tests. Run the ordinary development guide's CMake/CTest and IDF commands.
