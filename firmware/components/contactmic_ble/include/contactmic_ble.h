#ifndef CONTACTMIC_BLE_H
#define CONTACTMIC_BLE_H
#include "contactmic_protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
#define CM_TRANSPORT_MTU_LIMIT 247u
#define CM_TRANSPORT_BUFFER_SIZE (CM_TRANSPORT_MTU_LIMIT - 3u)

typedef struct { uint32_t index; int16_t value; } cm_source_sample;
typedef struct { uint64_t missed, i2c_errors, overruns; } cm_source_counters;

/* Nonblocking adapter boundary, not an acquisition implementation.
 * prepare may return BUSY while another task configures/settles/measures the source.
 * It must not perform I2C or wait here. On OK it reports a measured source rate.
 * start only enables publication; stop must synchronously gate publication and
 * clear queued samples without a blocking wait. Physical stop/configure work is
 * delegated to the source's owner. peek/consume must be bounded and preserve
 * a stable readable prefix until consume; never overwrite unread entries.
 * Counters are monotonic, saturating, lifetime source diagnostics.
 */
typedef struct {
    void *context;
    cm_result (*prepare)(void *, const cm_config *, uint32_t *rate_millihz, uint8_t *mode);
    cm_result (*start)(void *);
    void (*stop)(void *);
    bool (*peek)(void *, size_t offset, cm_source_sample *sample);
    void (*consume)(void *, size_t count);
    void (*counters)(void *, cm_source_counters *out);
} cm_source;

typedef struct {
    cm_source source;
    cm_status status;
    cm_source_counters observed, reset_base;
    uint16_t sequence;
    uint32_t last_end;
    uint8_t pending_flags;
    bool connected, subscribed, used_connection, have_last, exhausted, source_faulted;
    bool notify_failed_during_call, preparing;
} cm_transport;

typedef bool (*cm_notify_fn)(void *, const uint8_t *, size_t);

/* All transport calls belong to one serialized owner (the NimBLE host task).
 * Producer never calls these functions; it owns the independent source queue. */
cm_result cm_transport_init(cm_transport *, const cm_source *, uint32_t session_seed);
void cm_transport_connect(cm_transport *);
void cm_transport_disconnect(cm_transport *);
void cm_transport_subscribe(cm_transport *, bool enabled);
void cm_transport_mtu(cm_transport *, uint16_t effective_mtu);
void cm_transport_snapshot(cm_transport *);
cm_result cm_transport_control(cm_transport *, const cm_control *);
/* Record a later stack completion failure without inventing delivered samples. */
void cm_transport_notify_failed(cm_transport *);
/* Malformed requests leave state and reply untouched. Valid application errors
 * are encoded in a response. Return value describes framing, not application result. */
cm_result cm_transport_request(cm_transport *, const uint8_t *, size_t,
                               uint8_t *reply, size_t capacity, size_t *written);
/* At most one bounded packet per call. A rejected send drops that packet visibly. */
cm_result cm_transport_pump(cm_transport *, cm_notify_fn, void *notify_context);

#ifdef __cplusplus
}
#endif
#endif
