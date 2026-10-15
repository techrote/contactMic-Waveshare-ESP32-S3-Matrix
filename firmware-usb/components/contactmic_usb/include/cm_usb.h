#ifndef CM_USB_H
#define CM_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CMU_RATE 48000u
#define CMU_FRAME_SAMPLES 48u
#define CMU_FRAME_BYTES 96u
#define CMU_CONTROL_BYTES 128u
#define CMU_PREPARE_US 2000000ull
#define CMU_MAX_SOURCE_RATE 12000000u /* milli-Hz: resource bound, not promised ODR */

typedef enum { CMU_OK, CMU_INVALID, CMU_BUSY, CMU_IO, CMU_EXHAUSTED } cmu_result;
typedef enum { CMU_STOPPED, CMU_PREPARING, CMU_PRIMING, CMU_RUNNING, CMU_FAILED } cmu_state;
typedef enum { CMU_SAFE, CMU_MEDIUM, CMU_HIGH, CMU_AUTO } cmu_policy;
typedef enum { CMU_REASON_NONE, CMU_REASON_STOP, CMU_REASON_RESET,
    CMU_REASON_SUSPEND, CMU_REASON_DETACH, CMU_REASON_BACKEND,
    CMU_REASON_HEALTH, CMU_REASON_DEADLINE } cmu_reason;

typedef struct {
    uint32_t axis, range_g, policy, gain_milli, cutoff_millihz;
} cmu_config;
typedef struct { uint32_t index; int16_t value; } cmu_sample;
typedef struct {
    uint64_t intervals, reads, missed, i2c, overruns, faults;
} cmu_source_counts;

/* Single USB owner calls peek/consume/count/snapshot; only controller worker calls
 * lifecycle functions. Calibration/publication ownership is the backend's job.
 * These are the accepted acquisition interfaces, not another acquisition ring. */
typedef struct {
    void *context;
    cmu_result (*begin)(void *, const cmu_config *, uint32_t selected_mode);
    cmu_result (*ready)(void *, uint32_t *rate_millihz);
    cmu_result (*publish)(void *);
    bool (*published)(void *);
    cmu_result (*stop)(void *);
    bool (*peek)(void *, size_t, cmu_sample *);
    size_t (*consume)(void *, size_t);
    size_t (*count)(void *);
    bool (*snapshot)(void *, cmu_source_counts *);
} cmu_backend;

/* Atomic scalar mailboxes. Fields must only be accessed through core functions. */
typedef struct { uint32_t seq, generation, run, axis, range_g, mode; } cmu_command;
typedef struct { uint32_t seq, generation, state, rate, error; } cmu_reply;
typedef struct {
    uint64_t output_samples, startup_silence, starvation_silence, failed_silence;
    uint64_t source_gaps, gap_zeros, duplicate_source, large_gap_resets;
    uint64_t dsp_clips, raw_clips, resampler_resets, queue_discards;
    uint64_t clock_updates, clock_saturated, usb_short_writes, usb_late_frames;
    uint64_t transitions, rejected_controls, cdc_overflows, cdc_dropped_replies;
} cmu_counters;

typedef struct {
    cmu_backend backend;
    cmu_command command;
    cmu_reply reply;
    cmu_config config;
    cmu_counters counters; /* USB owner only; monotonic for boot lifetime */
    cmu_source_counts source, health_base;
    uint32_t generation, state, reason, rate, mode, auto_ceiling;
    bool requested, ever_running, source_snapshot_fresh, have_index, have_pending, have_pair, resync;
    cmu_sample pending;
    uint32_t expected_index, gap_remaining, target;
    uint32_t queue_high, queue_low;
    int32_t correction_ppm;
    double phase, left, right, previous_input, previous_output, pole;
    double occupancy_filter, integrator, correction;
    uint64_t state_started_us, health_started_us, last_frame_us;
    uint64_t generation_output_samples;
    /* Worker-only state. Never read these from the USB owner. */
    uint32_t worker_generation, worker_state;
    bool worker_run;
    uint64_t worker_started_us;
} cmu_core;

bool cmu_config_valid(const cmu_config *);
cmu_result cmu_init(cmu_core *, const cmu_backend *);
cmu_result cmu_configure(cmu_core *, const cmu_config *); /* stopped only */
cmu_result cmu_request_stream(cmu_core *, bool active, cmu_reason, uint64_t now_us);
void cmu_worker_step(cmu_core *, uint64_t now_us); /* controller, may do bounded I2C */
void cmu_tick(cmu_core *, uint64_t now_us); /* USB owner, no wait/heap/I2C */
void cmu_render(cmu_core *, uint8_t output[CMU_FRAME_BYTES], uint64_t now_us);
void cmu_usb_written(cmu_core *, size_t bytes);
/* Strict bounded ASCII command protocol. STATUS id; CONFIG id axis range policy
 * gain_milli cutoff_millihz. No streaming data ever appears on CDC. */
cmu_result cmu_command_line(cmu_core *, const char *, size_t, uint32_t *request_id);
void cmu_snapshot_source(cmu_core *);

#ifdef __cplusplus
}
#endif
#endif
