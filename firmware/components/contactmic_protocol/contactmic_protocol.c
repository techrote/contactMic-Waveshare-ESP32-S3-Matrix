#include "contactmic_protocol.h"
#include <string.h>

static uint16_t get16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16); }
static void put16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{ put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v)
{ put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

int cm_config_valid(const cm_config *c)
{ return c && c->axis <= CM_AXIS_Z && (c->range_g == 2 || c->range_g == 4)
         && c->policy <= CM_POLICY_AUTO; }
int cm_descriptor_valid(uint8_t d)
{ return !(d & 0xf8u) && (d & 3u) <= CM_AXIS_Z; }
uint8_t cm_descriptor(const cm_config *c)
{ return cm_config_valid(c) ? (uint8_t)(c->axis | (c->range_g == 4 ? 4u : 0u)) : 0xffu; }
size_t cm_audio_capacity(uint16_t mtu)
{
    if (mtu < 23) return 0;
    size_t n = ((size_t)mtu - 3 - CM_AUDIO_HEADER_SIZE) / 2;
    return n > CM_AUDIO_MAX_SAMPLES ? CM_AUDIO_MAX_SAMPLES : n;
}
cm_result cm_audio_encode(const cm_audio *f, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!f || !out || !written || !f->count || !f->rate_millihz ||
        (f->flags & ~7u) || !cm_descriptor_valid(f->descriptor)) return CM_INVALID;
    size_t n = CM_AUDIO_HEADER_SIZE + (size_t)f->count * 2;
    if (capacity < n) return CM_CAPACITY;
    out[0] = CM_PROTOCOL_MAJOR; out[1] = f->flags;
    put16(out + 2, f->sequence); put32(out + 4, f->first_index);
    put32(out + 8, f->rate_millihz); out[12] = f->count; out[13] = f->descriptor;
    put16(out + 14, 0);
    for (size_t i = 0; i < f->count; ++i) put16(out + 16 + i * 2, (uint16_t)f->samples[i]);
    *written = n; return CM_OK;
}
cm_result cm_audio_decode(const uint8_t *p, size_t n, cm_audio *out)
{
    if (!p || !out || n < CM_AUDIO_HEADER_SIZE) return CM_INVALID;
    if (p[0] != CM_PROTOCOL_MAJOR) return CM_UNSUPPORTED;
    if (!p[12] || n != CM_AUDIO_HEADER_SIZE + (size_t)p[12] * 2 ||
        (p[1] & ~7u) || !cm_descriptor_valid(p[13]) || get16(p + 14) || !get32(p + 8))
        return CM_INVALID;
    /* Decode only after complete validation; an invalid frame leaves output unchanged. */
    out->flags = p[1]; out->sequence = get16(p + 2); out->first_index = get32(p + 4);
    out->rate_millihz = get32(p + 8); out->count = p[12]; out->descriptor = p[13];
    for (size_t i = 0; i < out->count; ++i) {
        uint16_t raw = get16(p + 16 + i * 2);
        int32_t signed_value = raw >= 0x8000u ? (int32_t)raw - 65536 : raw;
        out->samples[i] = (int16_t)signed_value;
    }
    return CM_OK;
}
cm_result cm_control_decode(const uint8_t *p, size_t n, cm_control *out)
{
    if (!p || !out || n < CM_CONTROL_HEADER_SIZE || n > CM_CONTROL_MAX_REQUEST)
        return CM_INVALID;
    if (p[0] != CM_PROTOCOL_MAJOR) return CM_UNSUPPORTED;
    uint8_t op = p[1];
    if (op < CM_OP_CAPABILITIES || op > CM_OP_COMMIT_START) return CM_UNSUPPORTED;
    size_t wanted = op >= CM_OP_SET_AXIS && op <= CM_OP_SET_POLICY ? 1u :
                    op == CM_OP_COMMIT_START ? 4u : 0u;
    if (get16(p + 6) || get16(p + 4) != wanted || n != 8 + wanted) return CM_INVALID;
    if ((op == CM_OP_SET_AXIS && p[8] > CM_AXIS_Z) ||
        (op == CM_OP_SET_RANGE && p[8] != 2 && p[8] != 4) ||
        (op == CM_OP_SET_POLICY && p[8] > CM_POLICY_AUTO) ||
        (op == CM_OP_COMMIT_START && get32(p + 8) == 0)) return CM_INVALID;
    cm_control c = {0};
    c.opcode = op; c.request_id = get16(p + 2);
    if (wanted == 1) c.value = p[8];
    if (op == CM_OP_COMMIT_START) c.session_id = get32(p + 8);
    *out = c; return CM_OK;
}
cm_result cm_response_encode(uint8_t op, uint16_t id, cm_result result,
                             const uint8_t *payload, size_t n,
                             uint8_t *out, size_t cap, size_t *written)
{
    if (written) *written = 0;
    if (!out || !written || op < 1 || op > CM_OP_COMMIT_START ||
        result < CM_OK || result > CM_CAPACITY || n > CM_STATUS_SIZE || (n && !payload))
        return CM_INVALID;
    if (cap < 8 + n) return CM_CAPACITY;
    out[0] = 1; out[1] = (uint8_t)(op | 0x80u); put16(out + 2, id);
    out[4] = (uint8_t)result; out[5] = 0; put16(out + 6, (uint16_t)n);
    if (n) memcpy(out + 8, payload, n);
    *written = 8 + n; return CM_OK;
}
cm_result cm_status_encode(const cm_status *s, int event,
                           uint8_t *out, size_t cap, size_t *written)
{
    if (written) *written = 0;
    if (!s || !out || !written || !cm_config_valid(&s->config) || s->state > CM_STREAMING ||
        s->selected_mode > CM_POLICY_HIGH || s->phy > 3 || s->mtu < 23 ||
        (s->state != CM_STOPPED && (!s->session_id || !s->rate_millihz))) return CM_INVALID;
    size_t n = event ? CM_STATUS_EVENT_SIZE : CM_STATUS_SIZE;
    if (cap < n) return CM_CAPACITY;
    memset(out, 0, n); out[0] = 1; out[1] = event ? 1 : 0; put16(out + 2, (uint16_t)n);
    put32(out + 4, s->session_id); put32(out + 8, s->rate_millihz);
    put16(out + 12, s->mtu); out[14] = cm_descriptor(&s->config); out[15] = s->config.policy;
    out[16] = s->state; out[17] = s->selected_mode; out[18] = s->phy;
    if (!event) {
        put64(out + 20, s->source_misses); put64(out + 28, s->i2c_errors);
        put64(out + 36, s->ring_overruns); put64(out + 44, s->notify_rejections);
        put64(out + 52, s->packets_sent); put16(out + 60, s->data_length);
    }
    *written = n; return CM_OK;
}
