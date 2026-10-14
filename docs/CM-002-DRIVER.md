# CM-002: QMI8658C driver

## Scope and ownership

`contactmic_board.c` is the production, platform-independent register driver.
`contactmic_board_idf.c` adapts it to ESP-IDF v5.5.5's master I2C driver.
Neither file depends on BLE, an acquisition ISR, a sample ring or host audio.
`app_main` does not start the sensor. Physical performance remains unqualified.

The driver is single-owner/task-only. Configuration and startup include bounded
blocking bus operations and settling delays; never call them from an ISR. A
future acquisition task owns the driver; control requests must not perform its
I2C work in BLE callbacks.

## Sources and evidence limits

Register policy uses QST **QMI8658C**, document **13-52-27, Rev A**:
<https://www.qstcorp.com/upload/pdf/202210/13-52-27%20QMI8658C%20Datasheet%20Rev%20A%20(1).pdf>

Board pins use the accepted hardware contract and the official schematic:
<https://files.waveshare.com/wiki/ESP32-S3-Matrix/ESP32-S3-Matrix-Sch.pdf>

The manufacturer's Rev-A text was retrieved on 2026-10-14 after intermittent
fetch failures. PDF screenshot requests for the register/reset pages failed with
cache-miss errors: this implementation was checked against the retrieved textual
register descriptions, not a claimed successful visual rendering of those pages.
The board schematic was rendered. No older QMI8658C draft or QMI8658A register map
was substituted. No vendor document or build output is copied into the repository.

| Policy | Primary authority |
| --- | --- |
| Probe 7-bit addresses 0x6A and 0x6B | Section 1.5 / Table 3; SA0 high/low respectively |
| WHO_AM_I at 0x00 must be 0x05; revision at 0x01 is reported | Section 5.2 / Table 21 |
| CTRL1 = 0x40: auto-increment, little-endian, oscillator enabled | Section 5.3 / Table 22, p.30 |
| Accel scale codes 0 and 1; high/medium/safe ODR codes 0/1/3 | Table 22, pp.30–31 |
| Matching gyro ODR; both accel and gyro enabled | Table 22, pp.31/33; section 3.5 |
| LPFs and activity engines disabled | CTRL5/CTRL8, Table 22 |
| FIFO_CTRL = 0: bypass; CTRL7 bit5 controls INT2 DRDY | Table 23; sections 6.2/6.3 |
| AX/AY/AZ two-byte raw outputs at 0x35/0x37/0x39 | Section 5.6 / Table 25 |
| Reset command, witness and settling | Section 5.9 / Table 27; sections 7.3/7.4 |
| I2C clock no higher than 400 kHz | Section 16.3 / Table 43 |

### Vendor inconsistencies kept explicit

The reset-register definition (section 5.9, Table 27, p.40) specifies writing
**0xB0** to register 0x60. Section 7.4 (p.51) instead says **0x0B**. The driver
selects the dedicated register definition, writes 0xB0 only, waits 20 ms, and
requires the documented 0x4D == 0x80 reset witness before further configuration.
It never silently tries both values. A failed witness latches FAULT. This is an
explicit software policy resolving conflicting vendor prose, not physical proof
that a particular board reset. CM-007 must observe real reset/identification.

The overview and detailed tables also list different default revision IDs
(0x68 and 0x7C). Only WHO_AM_I is validated against a fixed value; the actual
revision byte is exposed, not rejected or fabricated.

The reset wait includes margin above the described 15 ms initialization. Start
waits 150 ms plus three nominal source intervals (section 7.3). These delays are
startup allowances; **actual source rate must still be measured by CM-003**.

## Lifecycle

Initialize a `cmq_idf_bus` with `{0}`, open it at at most 400000 Hz into a
`cmq_bus`, initialize a `cmq_device`, then:

`probe -> reset -> configure -> start -> read_selected -> stop`

`configure` accepts safe/medium/high, X/Y/Z, +/-2 g or +/-4 g and DRDY enable.
All three modes use 6DOF; nominal rates are 896.8, 3587.2 and 7174.4 Hz. Configure
keeps sensors stopped. `start` enables both sensors; `set_drdy` controls routing
without replacing other CTRL7 policy. Every configuration uses FIFO bypass.

Stop the producer before reconfiguration. Reconfiguration while RUNNING is
rejected without register writes. Invalid values are rejected before I/O.
Read failures preserve the caller's sample and can be counted/recovered by the
acquisition layer; they do not insert substitute samples.

Every configuration write is read back. A bus failure or mismatch latches FAULT,
performs one best-effort disable, and requires reset before another start. This
is not a transactional rollback: physical state can be unknown after a failed
bus transaction. A successful `stop` while faulted does not clear FAULT.

Probe checks both addresses, even when the first is valid. Two matching devices
are ambiguous and rejected; a probe-level bus fault also prevents claiming an
unambiguous identity. An expected absent-address NACK increments `probe_misses`,
not runtime I2C errors. Failed register transactions count as I2C errors.
Counters saturate at UINT64_MAX, remain available across reset/reconfiguration,
and are not silently cleared by recovery.

### Raw-read timing precondition

A selected-axis read is exactly one repeated-start register transaction and two
data bytes. In non-SyncSample DRDY mode the sensor can update registers while a
late read is in flight (section 6.3). **The acquisition layer must service and
qualify the DRDY-high timing window**; this driver does not claim an atomic
sensor snapshot merely because I2C returned two bytes. No locking-mode or
full-six-axis read is substituted. CM-003/CM-007 own timing/loss qualification.

## Adapter and test commands

The IDF adapter uses `i2c_master_probe`, `i2c_master_transmit_receive`, and
`i2c_master_transmit`, with a finite 20 ms error timeout. It allocates device
handles for both legal addresses, disables weak MCU pull-ups (the stock board
has external pull-ups), and configures no DRDY interrupt or LED. Failed cleanup
retains live handles for a retry rather than pretending they were freed.
See the pinned [Espressif I2C reference](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/peripherals/i2c.html).

From the repository root:

```sh
cmake -S tests/native -B build/native -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native --parallel 2
ctest --test-dir build/native --output-on-failure --no-tests=error
idf.py -C firmware set-target esp32s3
idf.py -C firmware build
```

The native test compiles the actual driver. It exercises both addresses, identity
and ambiguity, six rate/range combinations, start/stop/DRDY changes, all XYZ signed
extremes, no-I/O validation failures, saturating counters, reset errors, every
configuration write/read failure, readback mismatch and recovery. It is not a
physical sensor simulator proving achievable timing or pickup quality.
