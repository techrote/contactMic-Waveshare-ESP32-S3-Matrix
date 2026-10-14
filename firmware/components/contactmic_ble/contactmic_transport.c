#include "contactmic_ble.h"
#include <string.h>

static void increment(uint64_t *value) { if (*value != UINT64_MAX) ++*value; }
static bool source_valid(const cm_source *s)
{ return s && s->prepare && s->start && s->stop && s->peek && s->consume && s->counters; }
cm_result cm_transport_init(cm_transport *t, const cm_source *s, uint32_t seed)
{
    if (!t || (s && !source_valid(s))) return CM_INVALID;
    cm_source copy = {0};
    if (s) copy = *s;
    memset(t, 0, sizeof(*t)); t->source = copy;
    t->status.config = (cm_config){CM_AXIS_Z, 2, CM_POLICY_SAFE};
    t->status.mtu = 23; t->status.session_id = seed;
    return CM_OK;
}
static void stop(cm_transport *t)
{
    t->status.state = CM_STOPPED; t->preparing = false;
    if (t->source.stop) t->source.stop(t->source.context);
    t->pending_flags = 0; t->have_last = false;
}
void cm_transport_connect(cm_transport *t)
{
    if (!t) return;
    stop(t); t->connected = true; t->subscribed = false; t->used_connection = false;
    t->status.mtu = 23; t->status.phy = 0; t->status.data_length = 0;
    t->status.rate_millihz = 0; t->sequence = 0;
    cm_transport_snapshot(t);
}
void cm_transport_disconnect(cm_transport *t)
{
    if (!t) return;
    stop(t); t->connected = false; t->subscribed = false;
    t->status.mtu = 23; t->status.phy = 0; t->status.data_length = 0;
    cm_transport_snapshot(t);
}
void cm_transport_subscribe(cm_transport *t, bool enabled)
{
    if (!t) return;
    t->subscribed = t->connected && enabled;
    if (!t->subscribed && t->status.state == CM_STREAMING) stop(t);
}
void cm_transport_mtu(cm_transport *t, uint16_t mtu)
{ if (t && mtu >= 23) t->status.mtu = mtu; }
void cm_transport_snapshot(cm_transport *t)
{
    if (!t || !t->source.counters) return;
    cm_source_counters c = {0};
    t->source.counters(t->source.context, &c);
    /* Unexpected source counter regression is not silently accepted as a reset. */
    if (c.missed < t->observed.missed || c.i2c_errors < t->observed.i2c_errors ||
        c.overruns < t->observed.overruns) {
        t->source_faulted = true;
        if (t->status.state != CM_STOPPED) stop(t);
        return;
    }
    if (c.missed > t->observed.missed) t->pending_flags |= CM_FLAG_SOURCE_GAP;
    if (c.i2c_errors > t->observed.i2c_errors) t->pending_flags |= CM_FLAG_I2C_ERROR;
    if (c.overruns > t->observed.overruns) t->pending_flags |= CM_FLAG_RING_OVERRUN;
    t->observed = c;
    t->status.source_misses = c.missed - t->reset_base.missed;
    t->status.i2c_errors = c.i2c_errors - t->reset_base.i2c_errors;
    t->status.ring_overruns = c.overruns - t->reset_base.overruns;
}
cm_result cm_transport_control(cm_transport *t, const cm_control *r)
{
    if (!t || !r || r->opcode < 1 || r->opcode > CM_OP_COMMIT_START) return CM_INVALID;
    if (!t->connected) return CM_STATE;
    if (r->opcode == CM_OP_CAPABILITIES || r->opcode == CM_OP_GET_CONFIG) return CM_OK;
    if (r->opcode == CM_OP_STOP) { stop(t); cm_transport_snapshot(t); return CM_OK; }
    if (r->opcode == CM_OP_COMMIT_START) {
        if (t->status.state != CM_PREPARED || !t->subscribed || t->used_connection ||
            r->session_id != t->status.session_id || t->source_faulted) return CM_STATE;
        cm_result rc = t->source.start(t->source.context);
        if (rc != CM_OK) { stop(t); return rc; }
        t->used_connection = true; t->status.state = CM_STREAMING; return CM_OK;
    }
    if (t->status.state != CM_STOPPED) return CM_STATE;
    if (t->preparing && r->opcode != CM_OP_START) return CM_STATE;
    if (r->opcode == CM_OP_RESET_DIAGNOSTICS) {
        cm_transport_snapshot(t);
        if (t->source_faulted) return CM_STATE;
        t->reset_base = t->observed;
        t->status.source_misses = t->status.i2c_errors = t->status.ring_overruns = 0;
        t->status.notify_rejections = t->status.packets_sent = 0;
        t->pending_flags = 0; return CM_OK;
    }
    if (r->opcode == CM_OP_START) {
        /* V1 audio lacks a session id. One committed session per connection makes
         * restart/reconfiguration stale-data exclusion independent of OS callback ordering. */
        if (t->used_connection || t->exhausted || t->source_faulted) return CM_STATE;
        if (!source_valid(&t->source)) return CM_UNSUPPORTED;
        uint32_t rate = 0; uint8_t mode = 0;
        cm_result rc = t->source.prepare(t->source.context, &t->status.config, &rate, &mode);
        if (rc == CM_BUSY) { t->preparing = true; return rc; }
        if (rc != CM_OK) { stop(t); return rc; }
        t->preparing = false;
        if (!rate || mode > CM_POLICY_HIGH) { stop(t); return CM_INVALID; }
        if (t->status.session_id == UINT32_MAX) {
            t->exhausted = true; stop(t); return CM_STATE;
        }
        ++t->status.session_id;
        t->status.rate_millihz = rate; t->status.selected_mode = mode;
        t->sequence = 0; t->have_last = false; t->pending_flags = 0;
        cm_transport_snapshot(t);
        if (t->source_faulted) { stop(t); return CM_STATE; }
        t->status.state = CM_PREPARED; return CM_OK;
    }
    cm_config next = t->status.config;
    if (r->opcode == CM_OP_SET_AXIS) next.axis = r->value;
    else if (r->opcode == CM_OP_SET_RANGE) next.range_g = r->value;
    else if (r->opcode == CM_OP_SET_POLICY) next.policy = r->value;
    else return CM_UNSUPPORTED;
    if (!cm_config_valid(&next)) return CM_INVALID;
    t->status.config = next; return CM_OK;
}
cm_result cm_transport_pump(cm_transport *t, cm_notify_fn notify, void *ctx)
{
    if (!t || !notify) return CM_INVALID;
    if (!t->connected || !t->subscribed || t->status.state != CM_STREAMING) return CM_STATE;
    cm_transport_snapshot(t);
    if (t->status.state != CM_STREAMING) return CM_STATE;
    uint16_t mtu = t->status.mtu < CM_TRANSPORT_MTU_LIMIT ? t->status.mtu : CM_TRANSPORT_MTU_LIMIT;
    size_t capacity = cm_audio_capacity(mtu);
    cm_source_sample sample;
    if (!t->source.peek(t->source.context, 0, &sample)) return CM_BUSY;
    cm_audio f = {0};
    f.first_index = sample.index; f.rate_millihz = t->status.rate_millihz;
    f.sequence = t->sequence; f.descriptor = cm_descriptor(&t->status.config);
    f.flags = t->pending_flags;
    if ((t->have_last && sample.index != t->last_end) || (!t->have_last && sample.index != 0))
        f.flags |= CM_FLAG_SOURCE_GAP;
    for (size_t i = 0; i < capacity; ++i) {
        if (!t->source.peek(t->source.context, i, &sample) ||
            sample.index != f.first_index + (uint32_t)i) break;
        f.samples[i] = sample.value; ++f.count;
    }
    uint8_t data[CM_TRANSPORT_BUFFER_SIZE]; size_t length = 0;
    cm_result rc = cm_audio_encode(&f, data, sizeof(data), &length);
    if (rc != CM_OK) return rc;
    t->notify_failed_during_call = false;
    bool accepted = notify(ctx, data, length);
    t->source.consume(t->source.context, f.count);
    ++t->sequence; /* Includes rejected attempts, so a later peer sees sequence loss. */
    if (!accepted) {
        increment(&t->status.notify_rejections);
        t->pending_flags |= CM_FLAG_SOURCE_GAP;
        return CM_BUSY;
    }
    increment(&t->status.packets_sent);
    t->last_end = f.first_index + f.count; t->have_last = true;
    t->pending_flags = t->notify_failed_during_call ? CM_FLAG_SOURCE_GAP : 0;
    return CM_OK;
}

void cm_transport_notify_failed(cm_transport *t)
{
    if (!t) return;
    increment(&t->status.notify_rejections);
    t->pending_flags |= CM_FLAG_SOURCE_GAP;
    t->notify_failed_during_call = true;
}
cm_result cm_transport_request(cm_transport *t, const uint8_t *data, size_t length,
                               uint8_t *reply, size_t capacity, size_t *written)
{
    /* Reserve the worst-case reply before any state change. */
    if (!t || !reply || !written) return CM_INVALID;
    if (capacity < CM_CONTROL_MAX_RESPONSE) return CM_CAPACITY;
    cm_control request;
    cm_result rc = cm_control_decode(data, length, &request);
    if (rc != CM_OK) return rc;
    cm_result result = cm_transport_control(t, &request);
    uint8_t payload[CM_STATUS_SIZE]; size_t count = 0;
    if (result == CM_OK && request.opcode == CM_OP_CAPABILITIES) {
        const uint8_t capabilities[] = {1, 1, 7, 3, 15,
            (uint8_t)(3u | (source_valid(&t->source) ? 4u : 0u)), 247, 0};
        memcpy(payload, capabilities, sizeof(capabilities)); count = sizeof(capabilities);
    } else {
        cm_transport_snapshot(t);
        rc = cm_status_encode(&t->status, 0, payload, sizeof(payload), &count);
        if (rc != CM_OK) return rc;
    }
    return cm_response_encode(request.opcode, request.request_id, result,
                              payload, count, reply, capacity, written);
}
