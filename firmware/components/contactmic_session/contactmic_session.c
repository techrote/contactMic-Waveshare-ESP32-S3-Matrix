#include "contactmic_session.h"
#include <string.h>

#define GET(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define SET(p,v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define ADD(p) ((void)__atomic_add_fetch((p), 1u, __ATOMIC_SEQ_CST))
#define PREPARE_BUDGET_US 2000000ull

static uint32_t nominal(uint8_t mode)
{
    /* Accepted CM-002 6DOF candidates; never used as a measured rate. */
    const uint32_t rates[] = {896800u, 3587200u, 7174400u};
    return mode <= CM_POLICY_HIGH ? rates[mode] : 0;
}
static uint32_t pack(const cm_config *c)
{ return c->axis | ((uint32_t)c->range_g << 8) | ((uint32_t)c->policy << 16); }
static cm_config unpack(uint32_t word)
{ return (cm_config){(uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16)}; }
static void request(cm_session *s, uint32_t kind)
{
    ADD(&s->command.sequence);
    SET(&s->command.generation, s->generation);
    SET(&s->command.kind, kind);
    SET(&s->command.config, s->request_config);
    SET(&s->command.mode, s->request_mode);
    ADD(&s->command.sequence);
}
static bool read_command(cm_session *s, cm_session_command *out)
{
    uint32_t seq = GET(&s->command.sequence);
    if (seq & 1u) return false;
    *out = (cm_session_command){seq, GET(&s->command.generation),
        GET(&s->command.kind), GET(&s->command.config), GET(&s->command.mode)};
    return seq == GET(&s->command.sequence);
}
static void answer(cm_session *s, uint32_t state, uint32_t rate, cm_result result)
{
    ADD(&s->reply.sequence);
    SET(&s->reply.generation, s->worker_generation);
    SET(&s->reply.state, state);
    SET(&s->reply.rate, rate);
    SET(&s->reply.result, (uint32_t)result);
    ADD(&s->reply.sequence);
}
static bool read_reply(cm_session *s, cm_session_reply *out)
{
    uint32_t seq = GET(&s->reply.sequence);
    if (seq & 1u) return false;
    *out = (cm_session_reply){seq, GET(&s->reply.generation),
        GET(&s->reply.state), GET(&s->reply.rate), GET(&s->reply.result)};
    return seq == GET(&s->reply.sequence) && out->generation == s->generation;
}
static void stop_source(void *context)
{
    cm_session *s = context;
    /* This frontend is the only external ring consumer. No peek/consume can
     * overlap this callback. Gate first; the worker clears physical storage only
     * after seeing this command. Even a stale COMMIT completion stays invisible. */
    s->visible = false; s->active = false; s->prepared = false;
    s->monitoring = false;
    if (GET(&s->command.kind) != CMS_STOP && s->generation != UINT32_MAX)
        ++s->generation;
    request(s, CMS_STOP);
}
static cm_result prepare_source(void *context, const cm_config *config,
                                 uint32_t *rate, uint8_t *mode)
{
    cm_session *s = context;
    if (!cm_config_valid(config) || !rate || !mode) return CM_INVALID;
    if (s->visible) return CM_STATE;
    uint8_t selected = config->policy == CM_POLICY_AUTO ? s->auto_ceiling : config->policy;
    while (config->policy == CM_POLICY_AUTO && selected > CM_POLICY_SAFE &&
           nominal(selected) > s->capacity_millihz) --selected; /* at most two steps */
    if (!s->capacity_millihz || nominal(selected) > s->capacity_millihz) return CM_CAPACITY;
    uint32_t cfg = pack(config);
    if (!s->active || s->request_config != cfg || s->request_mode != selected) {
        if (s->generation == UINT32_MAX) return CM_STATE;
        ++s->generation;
        s->request_config = cfg; s->request_mode = selected;
        s->active = true; s->prepared = false;
        request(s, CMS_PREPARE);
        return CM_BUSY;
    }
    cm_session_reply r;
    if (!read_reply(s, &r)) return CM_BUSY;
    if (r.state == CMS_FAILED) return (cm_result)r.result;
    if (r.state != CMS_READY) return CM_BUSY;
    if (!r.rate) return CM_IO;
    if (r.rate > s->capacity_millihz) {
        if (config->policy == CM_POLICY_AUTO && selected > CM_POLICY_SAFE) {
            s->auto_ceiling = selected - 1u;
            s->last_reason = CMS_REASON_CAPACITY;
            if (s->transitions != UINT64_MAX) ++s->transitions;
            stop_source(s);
            return CM_BUSY;
        }
        return CM_CAPACITY;
    }
    s->prepared = true; *rate = r.rate; *mode = selected;
    return CM_OK;
}
static cm_result start_source(void *context)
{
    cm_session *s = context;
    cm_session_reply r;
    if (!s->active || !s->prepared || s->visible || !read_reply(s, &r) ||
        r.state != CMS_READY) return CM_STATE;
    if (!s->capacity_millihz || r.rate > s->capacity_millihz) return CM_CAPACITY;
    s->visible = true;
    request(s, CMS_COMMIT);
    return CM_OK; /* Publication waits for the tagged producer acknowledgment. */
}
static bool is_visible(cm_session *s)
{
    cm_session_reply r;
    return s->visible && read_reply(s, &r) && r.state == CMS_LIVE;
}
static bool peek_source(void *context, size_t offset, cm_source_sample *out)
{
    cm_session *s = context;
    if (!out || !is_visible(s)) return false;
    cm_acq_sample sample;
    if (!cm_acq_peek(s->acquisition, offset, &sample)) return false;
    *out = (cm_source_sample){sample.index, sample.value};
    return true;
}
static void consume_source(void *context, size_t count)
{
    cm_session *s = context;
    if (is_visible(s)) (void)cm_acq_consume(s->acquisition, count);
}
static void counters_source(void *context, cm_source_counters *out)
{
    cm_session *s = context;
    if (!out) return;
    (void)cm_acq_try_snapshot(s->acquisition, &s->cached);
    *out = (cm_source_counters){s->cached.inferred_missed_intervals,
                               s->cached.i2c_failures, s->cached.ring_overruns};
}
cm_result cm_session_init(cm_session *s, cm_acq *acq, const cm_session_backend *b,
                           uint8_t ceiling)
{
    if (!s || !acq || !b || !b->begin || !b->ready || !b->publish ||
        !b->published || !b->stop || ceiling > CM_POLICY_HIGH) return CM_INVALID;
    cm_session_backend copy = *b;
    memset(s, 0, sizeof(*s)); s->acquisition = acq; s->backend = copy;
    s->auto_ceiling = ceiling;
    return CM_OK;
}
cm_source cm_session_source(cm_session *s)
{
    if (!s) return (cm_source){0};
    return (cm_source){s, prepare_source, start_source, stop_source,
                        peek_source, consume_source, counters_source};
}
static void fail_worker(cm_session *s, cm_result cause)
{
    /* Failure can only arise before LIVE. No frontend reader can access the
     * ring, so stop once even when an abandoned peer never polls again. */
    cm_result cleanup = s->backend.stop(s->backend.context);
    s->worker_state = CMS_FAILED;
    answer(s, CMS_FAILED, 0, cleanup == CM_OK ? cause : CM_IO);
}
void cm_session_worker_step(cm_session *s, uint64_t now)
{
    if (!s) return;
    cm_session_command c;
    if (!read_command(s, &c)) return;
    if (c.generation != s->worker_generation ||
        (c.kind == CMS_STOP && s->worker_kind != CMS_STOP)) {
        cm_result rc = s->backend.stop(s->backend.context);
        s->worker_generation = c.generation;
        s->worker_kind = c.kind;
        s->worker_state = CMS_IDLE;
        answer(s, CMS_IDLE, 0, rc);
        if (rc != CM_OK) {
            s->worker_state = CMS_FAILED; answer(s, CMS_FAILED, 0, rc); return;
        }
        if (c.kind == CMS_STOP) return;
        cm_config cfg = unpack(c.config);
        if (!cm_config_valid(&cfg) || c.mode > CM_POLICY_HIGH) {
            fail_worker(s, CM_INVALID); return;
        }
        /* A stop/new prepare may arrive while this backend call is in flight.
         * No ring is externally readable until the same generation reaches LIVE. */
        rc = s->backend.begin(s->backend.context, &cfg, (uint8_t)c.mode);
        if (rc != CM_OK) { fail_worker(s, rc); return; }
        s->worker_started_us = now;
        s->worker_state = CMS_MEASURING;
        answer(s, CMS_MEASURING, 0, CM_BUSY);
        return;
    }
    if (c.kind == CMS_STOP || s->worker_state == CMS_FAILED) return;
    if (s->worker_state == CMS_MEASURING) {
        if (now < s->worker_started_us || now - s->worker_started_us >= PREPARE_BUDGET_US) {
            fail_worker(s, CM_IO); return;
        }
        uint32_t rate = 0;
        cm_result rc = s->backend.ready(s->backend.context, &rate);
        if (rc == CM_BUSY && now >= s->worker_started_us &&
            now - s->worker_started_us < PREPARE_BUDGET_US) return;
        if (rc != CM_OK || !rate) {
            fail_worker(s, CM_IO); return;
        }
        s->worker_state = CMS_READY; answer(s, CMS_READY, rate, CM_OK);
    }
    if (c.kind == CMS_COMMIT && s->worker_state == CMS_READY) {
        s->worker_kind = CMS_COMMIT;
        cm_result rc = s->backend.publish(s->backend.context);
        if (rc != CM_OK) { fail_worker(s, rc); return; }
        s->worker_state = CMS_LAUNCHING;
        s->worker_started_us = now;
    }
    if (s->worker_state == CMS_LAUNCHING) {
        /* Check the budget before the acknowledgment. A late producer reply
         * must not reopen publication after a timed-out or regressing clock. */
        if (now < s->worker_started_us ||
            now - s->worker_started_us >= PREPARE_BUDGET_US) {
            fail_worker(s, CM_IO); return;
        }
        if (s->backend.published(s->backend.context)) {
            s->worker_state = CMS_LIVE;
            answer(s, CMS_LIVE, GET(&s->reply.rate), CM_OK);
        }
    }
}
uint32_t cm_session_capacity(uint16_t mtu, uint32_t notifications)
{
    if (mtu > CM_TRANSPORT_MTU_LIMIT) mtu = CM_TRANSPORT_MTU_LIMIT;
    uint64_t rate = (uint64_t)cm_audio_capacity(mtu) * notifications * 1000u;
    return rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;
}
bool cm_session_monitor(cm_session *s, cm_transport *t, uint64_t now, uint32_t notifications)
{
    if (!s || !t) return false;
    s->capacity_millihz = cm_session_capacity(t->status.mtu, notifications);
    cm_transport_snapshot(t);
    if (t->status.state == CM_STOPPED) { s->monitoring = false; return false; }
    uint32_t reason = 0;
    if (t->status.rate_millihz > s->capacity_millihz) reason |= CMS_REASON_CAPACITY;
    cm_session_reply reply;
    if (read_reply(s, &reply) && reply.state == CMS_FAILED) reason |= CMS_REASON_BACKEND;
    if (t->status.state == CM_STREAMING) {
        if (!s->monitoring || s->monitored_session != t->status.session_id) {
            s->monitoring = true; s->monitored_session = t->status.session_id;
            s->monitor_us = now; s->health_base = t->observed;
            s->notify_base = t->status.notify_rejections;
        } else if (now < s->monitor_us || now - s->monitor_us >= 1000000u) {
            if (t->status.config.policy == CM_POLICY_AUTO) {
                if (t->observed.missed > s->health_base.missed) reason |= CMS_REASON_SOURCE;
                if (t->observed.i2c_errors > s->health_base.i2c_errors) reason |= CMS_REASON_I2C;
                if (t->observed.overruns > s->health_base.overruns) reason |= CMS_REASON_RING;
                if (t->status.notify_rejections > s->notify_base) reason |= CMS_REASON_NOTIFY;
            }
            s->monitor_us = now; s->health_base = t->observed;
            s->notify_base = t->status.notify_rejections;
        }
    }
    if (!reason) return false;
    uint8_t next = t->status.selected_mode;
    if (t->status.config.policy == CM_POLICY_AUTO && next > CM_POLICY_SAFE) {
        --next;
        while (next > CM_POLICY_SAFE && nominal(next) > s->capacity_millihz) --next;
        if (next < s->auto_ceiling) s->auto_ceiling = next;
    }
    /* A v1 committed stream cannot change metadata or restart on this connection.
     * Publish a stopped boundary and the next requested mode. Reconnection and
     * a new prepare/commit handshake are required; no historical counter reset. */
    const cm_control stop = {.opcode = CM_OP_STOP};
    (void)cm_transport_control(t, &stop);
    t->status.selected_mode = next; t->status.rate_millihz = 0;
    s->last_reason = reason;
    if (s->transitions != UINT64_MAX) ++s->transitions;
    return true;
}
