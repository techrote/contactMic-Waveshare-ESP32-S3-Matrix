#ifndef CONTACTMIC_PROTOCOL_H
#define CONTACTMIC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CM_PROTOCOL_MAJOR 1u
#define CM_AUDIO_HEADER_SIZE 16u
#define CM_AUDIO_MAX_SAMPLES 255u
#define CM_CONTROL_HEADER_SIZE 8u
#define CM_CONTROL_MAX_REQUEST 12u
#define CM_STATUS_SIZE 64u
#define CM_STATUS_EVENT_SIZE 20u
#define CM_CONTROL_MAX_RESPONSE (CM_CONTROL_HEADER_SIZE + CM_STATUS_SIZE)
#define CM_FLAG_SOURCE_GAP 1u
#define CM_FLAG_I2C_ERROR 2u
#define CM_FLAG_RING_OVERRUN 4u

/* Values are wire values, not sensor register encodings. */
typedef enum { CM_AXIS_X = 0, CM_AXIS_Y = 1, CM_AXIS_Z = 2 } cm_axis;
typedef enum { CM_POLICY_SAFE = 0, CM_POLICY_MEDIUM = 1,
               CM_POLICY_HIGH = 2, CM_POLICY_AUTO = 3 } cm_policy;
typedef enum { CM_OK = 0, CM_INVALID = 1, CM_UNSUPPORTED = 2,
               CM_STATE = 3, CM_BUSY = 4, CM_IO = 5, CM_CAPACITY = 6 } cm_result;
typedef enum { CM_STOPPED = 0, CM_PREPARED = 1, CM_STREAMING = 2 } cm_stream_state;
typedef enum { CM_OP_CAPABILITIES = 1, CM_OP_GET_CONFIG = 2,
               CM_OP_SET_AXIS = 3, CM_OP_SET_RANGE = 4, CM_OP_SET_POLICY = 5,
               CM_OP_START = 6, CM_OP_STOP = 7, CM_OP_RESET_DIAGNOSTICS = 8,
               CM_OP_COMMIT_START = 9 } cm_opcode;

typedef struct {
    uint8_t axis;
    uint8_t range_g;
    uint8_t policy;
} cm_config;

typedef struct {
    uint16_t sequence;
    uint32_t first_index;
    uint32_t rate_millihz;
    uint8_t flags;
    uint8_t descriptor;
    uint8_t count;
    int16_t samples[CM_AUDIO_MAX_SAMPLES];
} cm_audio;

typedef struct {
    uint8_t opcode;
    uint16_t request_id;
    uint8_t value;  /* axis/range/policy */
    uint32_t session_id; /* COMMIT_START only */
} cm_control;

typedef struct {
    uint32_t session_id;
    uint32_t rate_millihz;
    uint16_t mtu;
    cm_config config;
    uint8_t state;
    uint8_t selected_mode;
    uint8_t phy; /* 0 unknown, 1 1M, 2 2M, 3 coded */
    uint64_t source_misses;
    uint64_t i2c_errors;
    uint64_t ring_overruns;
    uint64_t notify_rejections;
    uint64_t packets_sent;
    uint16_t data_length; /* 0 unknown; a request is not an observation */
} cm_status;

int cm_config_valid(const cm_config *config);
int cm_descriptor_valid(uint8_t descriptor);
uint8_t cm_descriptor(const cm_config *config);
size_t cm_audio_capacity(uint16_t effective_mtu);
cm_result cm_audio_encode(const cm_audio *frame, uint8_t *out, size_t capacity,
                          size_t *written);
cm_result cm_audio_decode(const uint8_t *data, size_t length, cm_audio *frame);
cm_result cm_control_decode(const uint8_t *data, size_t length, cm_control *request);
cm_result cm_response_encode(uint8_t opcode, uint16_t request_id, cm_result result,
                             const uint8_t *payload, size_t length,
                             uint8_t *out, size_t capacity, size_t *written);
cm_result cm_status_encode(const cm_status *status, int event,
                           uint8_t *out, size_t capacity, size_t *written);

#ifdef __cplusplus
}
#endif
#endif
