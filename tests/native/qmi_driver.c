#include "contactmic_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef struct {
    bool present[2];
    uint8_t regs[2][256];
    size_t calls, fail_at, writes, reads, probes, last_n;
    uint8_t last_reg, last_value;
    uint32_t delay;
    bool corrupt_write, bad_reset;
} fake;
static bool fail(fake *f) { return ++f->calls == f->fail_at; }
static cmq_bus_result probe(void *ctx, uint8_t addr)
{
    fake *f = ctx; ++f->probes; CHECK(addr == 0x6a || addr == 0x6b);
    if (fail(f)) return CMQ_BUS_ERROR;
    return f->present[addr - 0x6a] ? CMQ_BUS_OK : CMQ_BUS_ABSENT;
}
static cmq_bus_result read_bytes(void *ctx, uint8_t addr, uint8_t reg, uint8_t *out, size_t n)
{
    fake *f = ctx; ++f->reads; f->last_n = n; f->last_reg = reg;
    CHECK(addr >= 0x6a && addr <= 0x6b); CHECK(n == 1 || n == 2);
    if (fail(f)) return CMQ_BUS_ERROR;
    if (!f->present[addr - 0x6a]) return CMQ_BUS_ABSENT;
    if (n == 2) CHECK((f->regs[addr - 0x6a][2] & 0xef) == 0x40); /* AI and LE; scoped INT2 gate. */
    memcpy(out, &f->regs[addr - 0x6a][reg], n); return CMQ_BUS_OK;
}
static cmq_bus_result write_byte(void *ctx, uint8_t addr, uint8_t reg, uint8_t value)
{
    fake *f = ctx; ++f->writes; f->last_reg = reg; f->last_value = value;
    CHECK(addr >= 0x6a && addr <= 0x6b);
    if (fail(f)) return CMQ_BUS_ERROR;
    uint8_t *r = f->regs[addr - 0x6a];
    if (reg == 0x60) {
        CHECK(value == 0xb0);
        uint8_t who = r[0], rev = r[1]; memset(r, 0, 256); r[0] = who; r[1] = rev;
        r[0x4d] = f->bad_reset ? 0 : 0x80; r[2] = 0x20;
    } else if (!f->corrupt_write) r[reg] = value;
    return CMQ_BUS_OK;
}
static void delay(void *ctx, uint32_t us) { ((fake *)ctx)->delay = us; }
static void setup(fake *f, cmq_device *d)
{
    memset(f, 0, sizeof(*f)); f->present[1] = true; f->regs[1][0] = 5; f->regs[1][1] = 0x7c;
    cmq_bus b = {f, probe, read_bytes, write_byte, delay};
    CHECK(cmq_init(d, &b, 400000) == CMQ_OK);
}
static void ready(fake *f, cmq_device *d)
{
    setup(f, d); CHECK(cmq_probe(d) == CMQ_OK); CHECK(d->address == 0x6b);
    CHECK(cmq_reset(d) == CMQ_OK); CHECK(f->delay >= 15200); CHECK(d->state == CMQ_IDLE);
}
static cmq_config configuration(void)
{ return (cmq_config){CMQ_Z, CMQ_SAFE, 2, true}; }
static void test_probe(void)
{
    fake f; cmq_device d; setup(&f, &d);
    CHECK(cmq_probe(&d) == CMQ_OK); CHECK(f.probes == 2); CHECK(f.reads == 2);
    CHECK(d.counters.probe_misses == 1); CHECK(d.counters.i2c_errors == 0);
    CHECK(d.revision == 0x7c); CHECK(d.who_am_i == 5);
    setup(&f, &d); f.present[0] = true; f.present[1] = false; f.regs[0][0] = 5; f.regs[0][1] = 0x12;
    CHECK(cmq_probe(&d) == CMQ_OK); CHECK(d.address == 0x6a); CHECK(d.revision == 0x12);
    setup(&f, &d); f.present[1] = false;
    CHECK(cmq_probe(&d) == CMQ_NOT_FOUND); CHECK(d.address == 0); CHECK(d.state == CMQ_UNPROBED);
    setup(&f, &d); f.regs[1][0] = 0xff;
    CHECK(cmq_probe(&d) == CMQ_IDENTITY); CHECK(d.counters.identity_rejections == 1);
    setup(&f, &d); f.present[0] = true; f.regs[0][0] = 6;
    CHECK(cmq_probe(&d) == CMQ_OK); CHECK(d.counters.identity_rejections == 1);
    setup(&f, &d); f.present[0] = true; f.regs[0][0] = 5;
    CHECK(cmq_probe(&d) == CMQ_AMBIGUOUS); CHECK(d.address == 0);
    for (size_t call = 1; call <= 4; ++call) {
        setup(&f, &d); f.fail_at = call;
        CHECK(cmq_probe(&d) == CMQ_IO); CHECK(d.address == 0); CHECK(d.counters.i2c_errors == 1);
        CHECK(cmq_reset(&d) == CMQ_STATE);
    }
}
static void test_configuration(void)
{
    const cmq_mode modes[] = {CMQ_SAFE, CMQ_MEDIUM, CMQ_HIGH};
    const uint8_t odrs[] = {3, 1, 0};
    const uint32_t rates[] = {896800, 3587200, 7174400};
    for (size_t m = 0; m < 3; ++m) for (uint8_t range = 2; range <= 4; range += 2) {
        fake f; cmq_device d; ready(&f, &d);
        cmq_config c = {CMQ_Z, modes[m], range, true};
        CHECK(cmq_configure(&d, &c) == CMQ_OK);
        CHECK(f.regs[1][2] == 0x40); CHECK(f.regs[1][3] == (odrs[m] | (range == 4 ? 0x10 : 0)));
        CHECK(f.regs[1][4] == odrs[m]); CHECK(f.regs[1][6] == 0); CHECK(f.regs[1][9] == 0);
        CHECK(f.regs[1][0x14] == 0); CHECK(f.regs[1][8] == 0x20);
        CHECK(cmq_nominal_rate_millihz(modes[m]) == rates[m]);
        CHECK(cmq_start(&d) == CMQ_OK); CHECK(f.regs[1][8] == 3); /* BOTH sensors. */
        CHECK(f.delay == cmq_settling_us(modes[m])); CHECK(f.delay > 150000);
        CHECK(cmq_configure(&d, &c) == CMQ_STATE); CHECK(cmq_probe(&d) == CMQ_STATE);
        CHECK(cmq_set_drdy(&d, false) == CMQ_OK); CHECK(f.regs[1][8] == 0x23);
        CHECK(cmq_set_drdy(&d, true) == CMQ_OK); CHECK(f.regs[1][8] == 3);
        CHECK(cmq_stop(&d) == CMQ_OK); CHECK(f.regs[1][8] == 0x20);
        CHECK(cmq_start(&d) == CMQ_OK); CHECK(f.regs[1][8] == 3);
    }
}
static void test_failures(void)
{
    cmq_config c = configuration();
    for (size_t call = 1; call <= 14; ++call) {
        fake f; cmq_device d; ready(&f, &d);
        f.fail_at = f.calls + call;
        CHECK(cmq_configure(&d, &c) == CMQ_IO); CHECK(d.state == CMQ_FAULT);
        CHECK(d.counters.i2c_errors == 1); CHECK(f.regs[1][8] == 0x20);
        CHECK(cmq_start(&d) == CMQ_STATE);
        CHECK(cmq_stop(&d) == CMQ_OK); CHECK(d.state == CMQ_FAULT);
        CHECK(cmq_reset(&d) == CMQ_OK); CHECK(cmq_configure(&d, &c) == CMQ_OK);
        CHECK(cmq_start(&d) == CMQ_OK);
    }
    fake f; cmq_device d; ready(&f, &d);
    c.range_g = 4; f.corrupt_write = true;
    CHECK(cmq_configure(&d, &c) == CMQ_VERIFY); CHECK(d.state == CMQ_FAULT);
    CHECK(d.counters.verification_errors == 1);
    setup(&f, &d); CHECK(cmq_probe(&d) == CMQ_OK); f.bad_reset = true;
    CHECK(cmq_reset(&d) == CMQ_VERIFY); CHECK(d.state == CMQ_FAULT);
    for (size_t call = 1; call <= 6; ++call) {
        setup(&f, &d); CHECK(cmq_probe(&d) == CMQ_OK); f.fail_at = f.calls + call;
        CHECK(cmq_reset(&d) == CMQ_IO); CHECK(d.state == CMQ_FAULT);
    }
    for (size_t call = 1; call <= 2; ++call) {
        ready(&f, &d); c = configuration(); CHECK(cmq_configure(&d, &c) == CMQ_OK);
        f.fail_at = f.calls + call;
        CHECK(cmq_start(&d) == CMQ_IO); CHECK(d.state == CMQ_FAULT);
        CHECK(f.regs[1][8] == 0x20);
    }
}
static void test_samples(void)
{
    fake f; cmq_device d; ready(&f, &d); cmq_config c = configuration();
    CHECK(cmq_configure(&d, &c) == CMQ_OK); CHECK(cmq_start(&d) == CMQ_OK);
    const int16_t values[] = {-32768, -1, 0, 1, 32767};
    for (int axis = 0; axis < 3; ++axis) for (size_t i = 0; i < 5; ++i) {
        uint8_t reg = (uint8_t)(0x35 + axis * 2); uint16_t raw = (uint16_t)values[i];
        f.regs[1][reg] = (uint8_t)raw; f.regs[1][reg + 1] = (uint8_t)(raw >> 8);
        size_t reads = f.reads; int16_t out = 42;
        CHECK(cmq_read_axis(&d, (cmq_axis)axis, &out) == CMQ_OK); CHECK(out == values[i]);
        CHECK(f.reads == reads + 1); CHECK(f.last_n == 2); CHECK(f.last_reg == reg);
    }
    int16_t out = 42; f.fail_at = f.calls + 1;
    CHECK(cmq_read_selected(&d, &out) == CMQ_IO); CHECK(out == 42); CHECK(d.state == CMQ_RUNNING);
    CHECK(cmq_read_selected(&d, &out) == CMQ_OK); CHECK(f.last_reg == 0x39);
    d.counters.samples_read = UINT64_MAX; d.counters.reads = UINT64_MAX;
    CHECK(cmq_read_selected(&d, &out) == CMQ_OK);
    CHECK(d.counters.samples_read == UINT64_MAX && d.counters.reads == UINT64_MAX);
}
static void test_invalid(void)
{
    fake f; cmq_device d; ready(&f, &d); cmq_config c = configuration();
    size_t calls = f.calls;
    CHECK(cmq_init(NULL, &d.bus, 400000) == CMQ_INVALID);
    CHECK(cmq_init(&d, NULL, 400000) == CMQ_INVALID);
    CHECK(cmq_init(&d, &d.bus, 400001) == CMQ_INVALID);
    CHECK(cmq_init(&d, &d.bus, 0) == CMQ_INVALID);
    for (int a = -1; a <= 4; ++a) if (a < 0 || a > 2) {
        c.axis = (cmq_axis)a; CHECK(cmq_configure(&d, &c) == CMQ_INVALID);
    }
    c = configuration(); c.range_g = 8; CHECK(cmq_configure(&d, &c) == CMQ_INVALID);
    c = configuration(); c.mode = (cmq_mode)-1; CHECK(cmq_configure(&d, &c) == CMQ_INVALID);
    c.mode = (cmq_mode)3; CHECK(cmq_configure(&d, &c) == CMQ_INVALID);
    CHECK(cmq_configure(&d, NULL) == CMQ_INVALID);
    CHECK(cmq_read_axis(&d, CMQ_X, NULL) == CMQ_INVALID);
    int16_t out = 77; CHECK(cmq_read_axis(&d, CMQ_X, &out) == CMQ_STATE); CHECK(out == 77);
    CHECK(f.calls == calls); CHECK(cmq_nominal_rate_millihz((cmq_mode)5) == 0);
    CHECK(cmq_settling_us((cmq_mode)-1) == 0);
}
static void test_diagnostics(void)
{
    fake f; cmq_device d; ready(&f,&d);cmq_config c=configuration();
    CHECK(cmq_configure(&d,&c)==CMQ_OK);CHECK(cmq_start(&d)==CMQ_OK);
    const uint8_t regs[12]={2,3,4,8,9,0x14,0x2d,0x2e,0x30,0x31,0x39,0x3a};
    uint8_t bytes[12];size_t writes=f.writes;uint64_t samples=d.counters.samples_read;
    for(size_t i=0;i<12;i++)f.regs[1][regs[i]]=(uint8_t)(i+16);
    CHECK(cmq_diagnostic_snapshot(&d,bytes)==CMQ_OK);
    for(size_t i=0;i<12;i++)CHECK(bytes[i]==i+16);
    CHECK(f.writes==writes && d.counters.samples_read==samples);
    for(size_t call=1;call<=12;call++) {
        f.fail_at=f.calls+call;memset(bytes,0xcc,12);
        CHECK(cmq_diagnostic_snapshot(&d,bytes)==CMQ_IO);
        for(size_t i=0;i<12;i++)CHECK(bytes[i]==0xcc);
    }
    CHECK(d.state==CMQ_RUNNING && f.writes==writes);
}
static void test_int2_gate(void)
{
    fake f;cmq_device d;ready(&f,&d);cmq_config c=configuration();
    CHECK(cmq_set_int2_output(&d,true)==CMQ_STATE);
    CHECK(cmq_configure(&d,&c)==CMQ_OK && f.regs[1][2]==0x40);
    CHECK(cmq_set_int2_output(&d,true)==CMQ_OK && f.regs[1][2]==0x50);
    CHECK(cmq_start(&d)==CMQ_OK);int16_t value;
    CHECK(cmq_read_selected(&d,&value)==CMQ_OK);
    size_t calls=f.calls;CHECK(cmq_set_int2_output(&d,false)==CMQ_STATE && f.calls==calls);
    CHECK(cmq_stop(&d)==CMQ_OK);CHECK(cmq_set_int2_output(&d,false)==CMQ_OK && f.regs[1][2]==0x40);
    for(size_t call=1;call<=2;call++) {
        ready(&f,&d);CHECK(cmq_configure(&d,&c)==CMQ_OK);f.fail_at=f.calls+call;
        CHECK(cmq_set_int2_output(&d,true)==CMQ_IO && d.state==CMQ_FAULT);
    }
    ready(&f,&d);CHECK(cmq_configure(&d,&c)==CMQ_OK);f.corrupt_write=true;
    CHECK(cmq_set_int2_output(&d,true)==CMQ_VERIFY && d.state==CMQ_FAULT);
}
int main(void)
{
    test_probe(); test_configuration(); test_failures(); test_samples(); test_invalid();test_diagnostics();test_int2_gate();
    puts("QMI driver: probe, register policy, recovery, boundary and fault-injection tests passed");
    return 0;
}
