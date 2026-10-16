#include "contactmic_board.h"
#include <string.h>

/* QST QMI8658C, document 13-52-27 Rev A, not QMI8658A or older drafts.
 * Sections 5.2/Table 21, 5.3/Table 22, 5.4/Table 23, 5.6/Table 25,
 * 5.9/Table 27 and 6.2/6.3. See docs/CM-002-DRIVER.md for source discrepancies.
 */
enum { WHO = 0x00, REV = 0x01, CTRL1 = 0x02, CTRL2 = 0x03, CTRL3 = 0x04,
       CTRL5 = 0x06, CTRL7 = 0x08, CTRL8 = 0x09, FIFO_CTRL = 0x14,
       AX_L = 0x35, RESET_RESULT = 0x4d, RESET = 0x60 };
static void increment(uint64_t *v) { if (*v != UINT64_MAX) ++*v; }
static int valid_axis(cmq_axis a) { return a >= CMQ_X && a <= CMQ_Z; }
static int valid_config(const cmq_config *c)
{
    return c && valid_axis(c->axis) && c->mode >= CMQ_SAFE && c->mode <= CMQ_HIGH &&
           (c->range_g == 2 || c->range_g == 4);
}
static cmq_result read_reg(cmq_device *d, uint8_t reg, uint8_t *out, size_t n)
{
    increment(&d->counters.reads);
    if (d->bus.read(d->bus.context, d->address, reg, out, n) != CMQ_BUS_OK) {
        increment(&d->counters.i2c_errors); return CMQ_IO;
    }
    return CMQ_OK;
}
static cmq_result write_reg(cmq_device *d, uint8_t reg, uint8_t value)
{
    increment(&d->counters.writes);
    if (d->bus.write(d->bus.context, d->address, reg, value) != CMQ_BUS_OK) {
        increment(&d->counters.i2c_errors); return CMQ_IO;
    }
    return CMQ_OK;
}
static cmq_result verified_write(cmq_device *d, uint8_t reg, uint8_t value)
{
    cmq_result rc = write_reg(d, reg, value);
    uint8_t observed = 0;
    if (rc == CMQ_OK) rc = read_reg(d, reg, &observed, 1);
    if (rc == CMQ_OK && observed != value) {
        increment(&d->counters.verification_errors); rc = CMQ_VERIFY;
    }
    return rc;
}
static cmq_result fault(cmq_device *d, cmq_result cause)
{
    d->state = CMQ_FAULT;
    /* One bounded best-effort disable, not a rollback or a claim that I2C succeeded. */
    (void)write_reg(d, CTRL7, 0x20);
    return cause;
}
uint32_t cmq_nominal_rate_millihz(cmq_mode mode)
{
    switch (mode) {
    case CMQ_HIGH: return 7174400u;
    case CMQ_MEDIUM: return 3587200u;
    case CMQ_SAFE: return 896800u;
    default: return 0;
    }
}
uint32_t cmq_settling_us(cmq_mode mode)
{
    uint32_t rate = cmq_nominal_rate_millihz(mode);
    /* Section 7.3: gyro wakeup 150 ms plus three source intervals.
     * This startup allowance is not a measured source rate or a hardware guarantee. */
    return rate ? 150000u + (3000000000u + (uint64_t)rate - 1u) / rate : 0;
}
cmq_result cmq_init(cmq_device *d, const cmq_bus *bus, uint32_t hz)
{
    if (!d || !bus || !bus->probe || !bus->read || !bus->write || !bus->delay_us ||
        !hz || hz > CMQ_I2C_MAX_HZ) return CMQ_INVALID;
    /* Copy first so an existing embedded bus argument cannot alias memset's output. */
    cmq_bus copy = *bus;
    memset(d, 0, sizeof(*d)); d->bus = copy; d->state = CMQ_UNPROBED;
    return CMQ_OK;
}
cmq_result cmq_probe(cmq_device *d)
{
    if (!d || !d->bus.probe || !d->bus.read) return CMQ_INVALID;
    if (d->state != CMQ_UNPROBED && d->state != CMQ_IDENTIFIED) return CMQ_STATE;
    uint8_t found = 0, address = 0, revision = 0;
    bool io_error = false, wrong_identity = false;
    d->state = CMQ_UNPROBED; d->address = 0; d->who_am_i = 0; d->revision = 0;
    for (uint8_t a = 0x6a; a <= 0x6b; ++a) {
        increment(&d->counters.probe_attempts);
        cmq_bus_result result = d->bus.probe(d->bus.context, a);
        if (result == CMQ_BUS_ABSENT) { increment(&d->counters.probe_misses); continue; }
        if (result != CMQ_BUS_OK) {
            increment(&d->counters.i2c_errors); io_error = true; continue;
        }
        d->address = a;
        uint8_t who = 0, rev = 0;
        if (read_reg(d, WHO, &who, 1) != CMQ_OK) { io_error = true; continue; }
        if (who != 0x05) {
            increment(&d->counters.identity_rejections); wrong_identity = true; continue;
        }
        if (read_reg(d, REV, &rev, 1) != CMQ_OK) { io_error = true; continue; }
        ++found; address = a; revision = rev;
    }
    d->address = 0;
    if (io_error) return CMQ_IO;
    if (found > 1) return CMQ_AMBIGUOUS;
    if (!found) return wrong_identity ? CMQ_IDENTITY : CMQ_NOT_FOUND;
    d->address = address; d->revision = revision; d->who_am_i = 0x05;
    d->state = CMQ_IDENTIFIED; return CMQ_OK;
}
cmq_result cmq_reset(cmq_device *d)
{
    if (!d || !d->bus.delay_us || !d->bus.write || !d->bus.read) return CMQ_INVALID;
    if (d->address != 0x6a && d->address != 0x6b) return CMQ_STATE;
    d->state = CMQ_FAULT;
    /* Table 27 explicitly specifies 0xB0; section 7.4 says 0x0B. Do not try
     * undocumented fallback writes. Require the documented completion witness. */
    cmq_result rc = write_reg(d, RESET, 0xb0);
    if (rc != CMQ_OK) return fault(d, rc);
    d->bus.delay_us(d->bus.context, 20000u);
    uint8_t witness = 0, who = 0, revision = 0;
    if ((rc = read_reg(d, RESET_RESULT, &witness, 1)) != CMQ_OK) return fault(d, rc);
    if (witness != 0x80) {
        increment(&d->counters.verification_errors); return fault(d, CMQ_VERIFY);
    }
    if ((rc = read_reg(d, WHO, &who, 1)) != CMQ_OK ||
        (rc = read_reg(d, REV, &revision, 1)) != CMQ_OK) return fault(d, rc);
    if (who != 0x05) {
        increment(&d->counters.identity_rejections); return fault(d, CMQ_IDENTITY);
    }
    /* AI=1, BE=0: two-byte selected-axis reads must advance in little-endian order. */
    if ((rc = verified_write(d, CTRL1, 0x40)) != CMQ_OK) return fault(d, rc);
    d->who_am_i = who; d->revision = revision; d->state = CMQ_IDLE;
    return CMQ_OK;
}
cmq_result cmq_configure(cmq_device *d, const cmq_config *c)
{
    if (!d || !valid_config(c)) return CMQ_INVALID;
    if (d->state != CMQ_IDLE && d->state != CMQ_CONFIGURED) return CMQ_STATE;
    uint8_t odr = c->mode == CMQ_HIGH ? 0 : c->mode == CMQ_MEDIUM ? 1 : 3;
    uint8_t scale = c->range_g == 4 ? 0x10 : 0;
    const uint8_t plan[][2] = {
        {CTRL7, 0x20}, {CTRL1, 0x40}, {CTRL2, (uint8_t)(scale | odr)},
        {CTRL3, odr}, {CTRL5, 0}, {CTRL8, 0}, {FIFO_CTRL, 0}
    };
    for (size_t i = 0; i < sizeof(plan) / sizeof(plan[0]); ++i) {
        cmq_result rc = verified_write(d, plan[i][0], plan[i][1]);
        if (rc != CMQ_OK) return fault(d, rc);
    }
    d->config = *c; d->state = CMQ_CONFIGURED; return CMQ_OK;
}
cmq_result cmq_start(cmq_device *d)
{
    if (!d) return CMQ_INVALID;
    if (d->state != CMQ_CONFIGURED) return CMQ_STATE;
    uint8_t value = d->config.drdy_enabled ? 0x03 : 0x23;
    cmq_result rc = verified_write(d, CTRL7, value);
    if (rc != CMQ_OK) return fault(d, rc);
    d->bus.delay_us(d->bus.context, cmq_settling_us(d->config.mode));
    d->state = CMQ_RUNNING; return CMQ_OK;
}
cmq_result cmq_stop(cmq_device *d)
{
    if (!d) return CMQ_INVALID;
    if (d->state != CMQ_RUNNING && d->state != CMQ_CONFIGURED && d->state != CMQ_FAULT)
        return CMQ_STATE;
    if (d->address != 0x6a && d->address != 0x6b) return CMQ_STATE;
    bool was_fault = d->state == CMQ_FAULT;
    cmq_result rc = verified_write(d, CTRL7, 0x20);
    if (rc != CMQ_OK) return fault(d, rc);
    if (!was_fault) d->state = CMQ_CONFIGURED;
    return CMQ_OK;
}
cmq_result cmq_set_drdy(cmq_device *d, bool enabled)
{
    if (!d) return CMQ_INVALID;
    if (d->state != CMQ_CONFIGURED && d->state != CMQ_RUNNING) return CMQ_STATE;
    uint8_t value = (d->state == CMQ_RUNNING ? 3u : 0u) | (enabled ? 0u : 0x20u);
    cmq_result rc = verified_write(d, CTRL7, value);
    if (rc != CMQ_OK) return fault(d, rc);
    d->config.drdy_enabled = enabled; return CMQ_OK;
}
cmq_result cmq_set_int2_output(cmq_device *d, bool enabled)
{
    if (!d) return CMQ_INVALID;
    if (d->state != CMQ_CONFIGURED) return CMQ_STATE;
    /* Waveshare QMI8658C INT2_ENABLE=0x10 conflicts with Rev-A reserved bits.
     * Explicit CM-009 hardware-contract exception, not a fallback register scan.
     * Preserve accepted AI=1 / BE=0; no INT1 output enable. */
    cmq_result rc = verified_write(d, CTRL1, enabled ? 0x50 : 0x40);
    return rc == CMQ_OK ? rc : fault(d, rc);
}
cmq_result cmq_read_axis(cmq_device *d, cmq_axis axis, int16_t *out)
{
    if (!d || !out || !valid_axis(axis)) return CMQ_INVALID;
    if (d->state != CMQ_RUNNING) return CMQ_STATE;
    uint8_t data[2];
    cmq_result rc = read_reg(d, (uint8_t)(AX_L + 2 * axis), data, sizeof(data));
    if (rc != CMQ_OK) return rc;
    uint16_t raw = (uint16_t)((uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8));
    *out = (int16_t)(raw >= 0x8000u ? (int32_t)raw - 65536 : raw);
    increment(&d->counters.samples_read); return CMQ_OK;
}
cmq_result cmq_read_selected(cmq_device *d, int16_t *out)
{ return d ? cmq_read_axis(d, d->config.axis, out) : CMQ_INVALID; }

cmq_result cmq_diagnostic_snapshot(cmq_device *d, uint8_t out[12])
{
    if (!d || !out) return CMQ_INVALID;
    if (d->state != CMQ_RUNNING) return CMQ_STATE;
    const uint8_t regs[12] = {CTRL1, CTRL2, CTRL3, CTRL7, CTRL8, FIFO_CTRL,
        0x2d, 0x2e, 0x30, 0x31, (uint8_t)(AX_L + 2 * d->config.axis),
        (uint8_t)(AX_L + 2 * d->config.axis + 1)};
    uint8_t snapshot[12];
    for (size_t i = 0; i < sizeof(snapshot); ++i) {
        cmq_result rc = read_reg(d, regs[i], &snapshot[i], 1);
        if (rc != CMQ_OK) return rc;
    }
    memcpy(out, snapshot, sizeof(snapshot));
    return CMQ_OK;
}
