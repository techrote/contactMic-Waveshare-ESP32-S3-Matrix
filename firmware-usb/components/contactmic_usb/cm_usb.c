#include "cm_usb.h"
#include <limits.h>
#include <math.h>
#include <string.h>

#define GET(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define SET(p,v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define BUMP(p) ((void)__atomic_add_fetch((p), 1u, __ATOMIC_SEQ_CST))
enum { W_IDLE, W_MEASURE, W_PUBLISH, W_LIVE, W_FAILED };

static void add(uint64_t *p, uint64_t n) { *p = UINT64_MAX - *p < n ? UINT64_MAX : *p + n; }
static double clamp(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

bool cmu_config_valid(const cmu_config *c)
{
    return c && c->axis <= 2u && (c->range_g == 2u || c->range_g == 4u) &&
        c->policy <= CMU_AUTO && c->gain_milli <= 64000u && c->cutoff_millihz <= 100000u;
}
static void command(cmu_core *c, bool run)
{
    BUMP(&c->command.seq);
    SET(&c->command.generation, c->generation);
    SET(&c->command.run, run ? 1u : 0u);
    SET(&c->command.axis, c->config.axis);
    SET(&c->command.range_g, c->config.range_g);
    SET(&c->command.mode, c->mode);
    BUMP(&c->command.seq);
}
static bool read_command(cmu_core *c, cmu_command *out)
{
    uint32_t seq = GET(&c->command.seq);
    if (seq & 1u) return false;
    *out = (cmu_command){seq, GET(&c->command.generation), GET(&c->command.run),
        GET(&c->command.axis), GET(&c->command.range_g), GET(&c->command.mode)};
    return seq == GET(&c->command.seq);
}
static void reply(cmu_core *c, uint32_t state, uint32_t rate, cmu_result error)
{
    BUMP(&c->reply.seq);
    SET(&c->reply.generation, c->worker_generation);
    SET(&c->reply.state, state); SET(&c->reply.rate, rate); SET(&c->reply.error, (uint32_t)error);
    BUMP(&c->reply.seq);
}
static bool read_reply(cmu_core *c, cmu_reply *out)
{
    uint32_t seq = GET(&c->reply.seq);
    if (seq & 1u) return false;
    *out = (cmu_reply){seq, GET(&c->reply.generation), GET(&c->reply.state),
        GET(&c->reply.rate), GET(&c->reply.error)};
    return seq == GET(&c->reply.seq) && out->generation == c->generation;
}
static void reset_dsp(cmu_core *c)
{
    c->phase = c->left = c->right = c->previous_input = c->previous_output = 0;
    c->have_pair = false;
    add(&c->counters.resampler_resets, 1);
}
void cmu_snapshot_source(cmu_core *c)
{
    if (c) c->source_snapshot_fresh = c->backend.snapshot(c->backend.context, &c->source);
}
cmu_result cmu_init(cmu_core *c, const cmu_backend *b)
{
    if (!c || !b || !b->begin || !b->ready || !b->publish || !b->published ||
        !b->stop || !b->peek || !b->consume || !b->count || !b->snapshot) return CMU_INVALID;
    cmu_backend copy = *b;
    memset(c, 0, sizeof(*c)); c->backend = copy;
    c->config = (cmu_config){2, 2, CMU_SAFE, 1000, 20000};
    c->auto_ceiling = CMU_SAFE; /* Unqualified USB-only bootstrap, not BLE default. */
    c->queue_low = UINT32_MAX;
    return CMU_OK;
}
cmu_result cmu_configure(cmu_core *c, const cmu_config *config)
{
    if (!c || !cmu_config_valid(config)) return CMU_INVALID;
    if (c->requested) return CMU_BUSY;
    c->config = *config;
    return CMU_OK;
}
cmu_result cmu_request_stream(cmu_core *c, bool active, cmu_reason reason, uint64_t now)
{
    if (!c) return CMU_INVALID;
    /* Gate first. Only this owner can consume; worker cannot clear behind a read. */
    c->requested = false; c->ever_running = false;
    reset_dsp(c);
    c->have_index = c->have_pending = c->resync = false;
    c->expected_index = c->gap_remaining = 0;
    c->rate = 0; c->generation_output_samples = 0;
    c->last_frame_us = 0; c->reason = (uint32_t)reason;
    c->integrator = c->correction = c->occupancy_filter = 0;
    c->correction_ppm = 0;
    if (c->generation != UINT32_MAX) ++c->generation;
    else if (active) {
        c->state = CMU_FAILED; command(c, false); return CMU_EXHAUSTED;
    }
    c->mode = c->config.policy == CMU_AUTO ? c->auto_ceiling : c->config.policy;
    c->state = active ? CMU_PREPARING : CMU_STOPPED;
    c->state_started_us = c->health_started_us = now;
    cmu_snapshot_source(c); c->health_base = c->source;
    c->requested = active;
    command(c, active);
    add(&c->counters.transitions, 1);
    return CMU_OK;
}
static void fail_worker(cmu_core *c, cmu_result error)
{
    cmu_result cleanup = c->backend.stop(c->backend.context);
    c->worker_state = W_FAILED;
    reply(c, W_FAILED, 0, cleanup == CMU_OK ? error : CMU_IO);
}
void cmu_worker_step(cmu_core *c, uint64_t now)
{
    if (!c) return;
    cmu_command cmd;
    if (!read_command(c, &cmd)) return;
    if (cmd.generation != c->worker_generation || (cmd.run != 0) != c->worker_run) {
        cmu_result r = c->backend.stop(c->backend.context);
        c->worker_generation = cmd.generation; c->worker_run = cmd.run != 0;
        c->worker_state = W_IDLE; reply(c, W_IDLE, 0, r);
        if (r != CMU_OK) { c->worker_state = W_FAILED; reply(c, W_FAILED, 0, r); return; }
        if (!cmd.run) return;
        cmu_config config = {cmd.axis, cmd.range_g, cmd.mode, 1000, 20000};
        if (!cmu_config_valid(&config) || cmd.mode > CMU_HIGH) { fail_worker(c, CMU_INVALID); return; }
        r = c->backend.begin(c->backend.context, &config, cmd.mode);
        if (r != CMU_OK) { fail_worker(c, r); return; }
        c->worker_state = W_MEASURE; c->worker_started_us = now;
        reply(c, W_MEASURE, 0, CMU_BUSY); return;
    }
    if (!cmd.run || c->worker_state == W_FAILED || c->worker_state == W_LIVE) return;
    if (now < c->worker_started_us || now - c->worker_started_us >= CMU_PREPARE_US) {
        fail_worker(c, CMU_IO); return;
    }
    if (c->worker_state == W_MEASURE) {
        uint32_t rate = 0;
        cmu_result r = c->backend.ready(c->backend.context, &rate);
        if (r == CMU_BUSY) return;
        if (r != CMU_OK || rate < 100000u || rate > CMU_MAX_SOURCE_RATE) { fail_worker(c, CMU_IO); return; }
        r = c->backend.publish(c->backend.context);
        if (r != CMU_OK) { fail_worker(c, r); return; }
        c->worker_state = W_PUBLISH; c->worker_started_us = now;
        reply(c, W_PUBLISH, rate, CMU_BUSY);
    }
    /* Check the deadline above BEFORE trusting any late acknowledgment. */
    if (c->worker_state == W_PUBLISH && c->backend.published(c->backend.context)) {
        c->worker_state = W_LIVE;
        reply(c, W_LIVE, GET(&c->reply.rate), CMU_OK);
    }
}
static void fail_frontend(cmu_core *c, cmu_reason reason)
{
    c->state = CMU_FAILED; c->reason = (uint32_t)reason;
    reset_dsp(c); command(c, false);
    /* requested remains true: endpoint still open, controls must not reconfigure. */
}
static double filter(cmu_core *c, int16_t sample)
{
    if (sample == INT16_MIN || sample == INT16_MAX) add(&c->counters.raw_clips, 1);
    double x = (double)sample / 32768.0;
    double y = x;
    if (c->config.cutoff_millihz) {
        y = x - c->previous_input + c->pole * c->previous_output;
        c->previous_input = x; c->previous_output = y;
    }
    y *= (double)c->config.gain_milli / 1000.0;
    if (y < -1 || y > 1) add(&c->counters.dsp_clips, 1);
    return clamp(y, -1, 1);
}
static bool next_source(cmu_core *c, double *value)
{
    /* At most four stale source entries per request; never an unbounded scan. */
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        if (!c->have_pending) {
            if (!c->backend.peek(c->backend.context, 0, &c->pending)) return false;
            if (c->backend.consume(c->backend.context, 1) != 1) return false;
            c->have_pending = true;
        }
        if (!c->have_index) { c->expected_index = 0; c->have_index = true; }
        if (c->gap_remaining) {
            --c->gap_remaining; ++c->expected_index;
            add(&c->counters.gap_zeros, 1); *value = filter(c, 0); return true;
        }
        uint32_t gap = c->pending.index - c->expected_index;
        if (gap >= 0x80000000u) {
            c->have_pending = false; add(&c->counters.duplicate_source, 1); continue;
        }
        if (gap) {
            add(&c->counters.source_gaps, gap);
            uint32_t limit = c->rate / 50000u; /* 20 ms, additionally capped at 32 intervals */
            if (limit > 32u) limit = 32u;
            if (gap > limit) {
                c->expected_index = c->pending.index;
                c->resync = true; add(&c->counters.large_gap_resets, 1); return false;
            }
            c->gap_remaining = gap;
            continue;
        }
        *value = filter(c, c->pending.value);
        ++c->expected_index; c->have_pending = false; return true;
    }
    return false;
}
void cmu_tick(cmu_core *c, uint64_t now)
{
    if (!c) return;
    cmu_snapshot_source(c);
    if (!c->requested || c->state == CMU_FAILED) return;
    cmu_reply r;
    if (read_reply(c, &r)) {
        if (r.state == W_FAILED) { fail_frontend(c, CMU_REASON_BACKEND); return; }
        if (c->state == CMU_PREPARING && r.state == W_LIVE) {
            if (!r.rate || c->config.cutoff_millihz >= r.rate / 2u) { fail_frontend(c, CMU_REASON_BACKEND); return; }
            c->rate = r.rate;
            c->pole = exp(-6.2831853071795864769 * c->config.cutoff_millihz / c->rate);
            c->target = c->rate / 50000u; /* 20 ms source reserve */
            if (c->target < 8u) c->target = 8u;
            if (c->target > 192u) c->target = 192u;
            c->state = CMU_PRIMING; c->state_started_us = now;
        }
    }
    if (c->state == CMU_PREPARING) {
        /* Total frontend bound also covers a worker which never runs. */
        if (now < c->state_started_us || now - c->state_started_us >= 2u * CMU_PREPARE_US)
            fail_frontend(c, CMU_REASON_DEADLINE);
        return;
    }
    size_t queued = c->backend.count(c->backend.context);
    if (queued > c->queue_high) c->queue_high = (uint32_t)queued;
    if (queued < c->queue_low) c->queue_low = (uint32_t)queued;
    if (queued > 224u) {
        size_t drop = queued - c->target;
        add(&c->counters.queue_discards, c->backend.consume(c->backend.context, drop));
        reset_dsp(c); c->state = CMU_PRIMING; c->state_started_us = now;
        queued = c->backend.count(c->backend.context);
    }
    if (c->state == CMU_PRIMING) {
        if (now < c->state_started_us || now - c->state_started_us >= CMU_PREPARE_US) {
            fail_frontend(c, CMU_REASON_DEADLINE); return;
        }
        if (queued >= c->target + 2u) {
            c->resync = false;
            if (!next_source(c, &c->left) || !next_source(c, &c->right)) {
                reset_dsp(c); return;
            }
            c->have_pair = true; c->ever_running = true; c->state = CMU_RUNNING;
            c->health_base = c->source; c->health_started_us = now;
        }
    }
    if (c->state == CMU_RUNNING &&
        (now < c->health_started_us || now - c->health_started_us >= 1000000u)) {
        bool unhealthy = c->source.missed > c->health_base.missed || c->source.i2c > c->health_base.i2c ||
            c->source.overruns > c->health_base.overruns || c->source.faults > c->health_base.faults;
        c->health_base = c->source; c->health_started_us = now;
        if (unhealthy && c->config.policy == CMU_AUTO) {
            if (c->mode) {
                c->auto_ceiling = c->mode - 1u;
                (void)cmu_request_stream(c, true, CMU_REASON_HEALTH, now);
            } else fail_frontend(c, CMU_REASON_HEALTH);
        }
    }
}
static void servo(cmu_core *c)
{
    double occupied = (double)c->backend.count(c->backend.context) + 2.0;
    c->occupancy_filter += ((occupied - c->target) - c->occupancy_filter) * 0.02;
    double seconds_error = c->occupancy_filter * 1000.0 / c->rate;
    double proposed_i = clamp(c->integrator + seconds_error * 0.00025, -0.03, 0.03);
    double proposed = 1.5 * seconds_error + proposed_i;
    if (fabs(proposed) < 0.03 || proposed * seconds_error < 0) c->integrator = proposed_i;
    c->correction = clamp(1.5 * seconds_error + c->integrator, -0.03, 0.03);
    c->correction_ppm = (int32_t)lrint(c->correction * 1000000.0);
    if (fabs(c->correction) >= 0.03) add(&c->counters.clock_saturated, 1);
    add(&c->counters.clock_updates, 1);
}
void cmu_render(cmu_core *c, uint8_t output[CMU_FRAME_BYTES], uint64_t now)
{
    if (!output) return;
    memset(output, 0, CMU_FRAME_BYTES);
    if (!c || !c->requested) return;
    cmu_tick(c, now);
    if (c->last_frame_us && now > c->last_frame_us && now - c->last_frame_us >= 2000u)
        add(&c->counters.usb_late_frames, (now - c->last_frame_us) / 1000u - 1u);
    c->last_frame_us = now;
    add(&c->counters.output_samples, CMU_FRAME_SAMPLES);
    add(&c->generation_output_samples, CMU_FRAME_SAMPLES);
    if (c->state != CMU_RUNNING) {
        add(c->state == CMU_FAILED ? &c->counters.failed_silence :
            c->ever_running ? &c->counters.starvation_silence : &c->counters.startup_silence, CMU_FRAME_SAMPLES);
        return;
    }
    servo(c);
    double step = (double)c->rate / (CMU_RATE * 1000.0) * (1.0 + c->correction);
    for (size_t i = 0; i < CMU_FRAME_SAMPLES; ++i) {
        while (c->phase >= 1.0) { /* <=1 source advance/output at the validated rate bound */
            c->phase -= 1.0; c->left = c->right;
            if (!next_source(c, &c->right)) {
                add(&c->counters.starvation_silence, CMU_FRAME_SAMPLES - i);
                reset_dsp(c); c->state = CMU_PRIMING; c->state_started_us = now;
                return;
            }
        }
        double y = c->left + (c->right - c->left) * c->phase;
        long sample = lrint(y * 32768.0);
        if (sample > INT16_MAX) sample = INT16_MAX;
        if (sample < INT16_MIN) sample = INT16_MIN;
        uint16_t bits = (uint16_t)(int16_t)sample;
        output[2u*i] = (uint8_t)bits; output[2u*i+1u] = (uint8_t)(bits >> 8);
        c->phase += step;
    }
}
void cmu_usb_written(cmu_core *c, size_t bytes)
{
    if (c && bytes != CMU_FRAME_BYTES) add(&c->counters.usb_short_writes, 1);
}
static bool number(const char **p, const char *end, uint32_t *out)
{
    while (*p != end && **p == ' ') ++*p;
    if (*p == end || **p < '0' || **p > '9') return false;
    uint32_t value = 0;
    while (*p != end && **p >= '0' && **p <= '9') {
        uint32_t digit = (uint32_t)(**p - '0');
        if (value > (UINT32_MAX - digit) / 10u) return false;
        value = value * 10u + digit; ++*p;
    }
    if (*p != end && **p != ' ') return false;
    *out = value; return true;
}
cmu_result cmu_command_line(cmu_core *c, const char *line, size_t length, uint32_t *id)
{
    if (!c || !line || !id) return CMU_INVALID;
    *id = 0;
    cmu_result result = CMU_INVALID;
    if (length && length < CMU_CONTROL_BYTES && !memchr(line, '\0', length)) {
        bool status = length >= 7 && !memcmp(line, "STATUS ", 7);
        bool config = length >= 7 && !memcmp(line, "CONFIG ", 7);
        const char *p = line + (length >= 7 ? 7 : 0), *end = line + length;
        if ((status || config) && number(&p, end, id) && *id) {
            cmu_config value;
            bool valid = status || (number(&p, end, &value.axis) && number(&p, end, &value.range_g) &&
                number(&p, end, &value.policy) && number(&p, end, &value.gain_milli) &&
                number(&p, end, &value.cutoff_millihz));
            while (p != end && *p == ' ') ++p;
            if (valid && p == end) result = status ? CMU_OK : cmu_configure(c, &value);
        }
    }
    if (result != CMU_OK) add(&c->counters.rejected_controls, 1);
    return result;
}
