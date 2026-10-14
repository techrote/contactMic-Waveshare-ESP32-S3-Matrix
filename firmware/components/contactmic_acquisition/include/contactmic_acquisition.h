#ifndef CONTACTMIC_ACQUISITION_H
#define CONTACTMIC_ACQUISITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CM_ACQ_RING_CAPACITY 256u

typedef enum {
    CM_ACQ_OK = 0,
    CM_ACQ_INVALID,
    CM_ACQ_STATE,
    CM_ACQ_IO,
    CM_ACQ_SOURCE_FAULT,
    CM_ACQ_RING_FULL
} cm_acq_result;

typedef enum {
    CM_ACQ_READ_OK = 0,
    CM_ACQ_READ_IO,
    CM_ACQ_READ_FAULT
} cm_acq_read_result;

typedef enum {
    CM_ACQ_POLICY_SAFE = 0,
    CM_ACQ_POLICY_MEDIUM = 1,
    CM_ACQ_POLICY_HIGH = 2,
    CM_ACQ_POLICY_AUTO = 3
} cm_acq_policy;

typedef enum {
    CM_ACQ_MODE_SAFE = 0,
    CM_ACQ_MODE_MEDIUM = 1,
    CM_ACQ_MODE_HIGH = 2
} cm_acq_mode;

typedef struct {
    uint32_t index;
    int16_t value;
} cm_acq_sample;

typedef struct {
    uint64_t drdy_edges;
    uint64_t successful_reads;
    uint64_t inferred_missed_intervals;
    uint64_t i2c_failures;
    uint64_t source_faults;
    uint64_t ring_overruns;
} cm_acq_counters;

typedef cm_acq_read_result (*cm_acq_read_fn)(void *context, int16_t *sample);

typedef struct {
    cm_acq_sample slots[CM_ACQ_RING_CAPACITY];
    uint32_t head;
    uint32_t tail;
} cm_acq_ring;

typedef struct {
    void *read_context;
    cm_acq_read_fn read;
    cm_acq_ring ring;
    cm_acq_counters counters;
    uint32_t counter_seq;
    uint32_t next_index;
    uint32_t measured_rate_millihz;
    uint32_t running;
} cm_acq;

typedef struct {
    uint64_t start_us;
    uint64_t start_edges;
} cm_acq_rate_window;

/*
 * The core is a single-producer/single-consumer boundary.
 * - One acquisition owner calls start/stop/service and is the only counter writer.
 * - One transport consumer may call peek/consume concurrently.
 * - Lifecycle reset requires producer and consumer to be quiesced.
 * The ring never overwrites unread entries; a full ring drops the new sample and
 * records an overrun, preserving the existing readable prefix.
 */
cm_acq_result cm_acq_init(cm_acq *acq, cm_acq_read_fn read, void *read_context);
cm_acq_result cm_acq_start_session(cm_acq *acq);
cm_acq_result cm_acq_stop_session(cm_acq *acq);
cm_acq_result cm_acq_service_edges(cm_acq *acq, uint32_t accumulated_edges);

size_t cm_acq_count(const cm_acq *acq);
bool cm_acq_peek(const cm_acq *acq, size_t offset, cm_acq_sample *sample);
size_t cm_acq_consume(cm_acq *acq, size_t count);
void cm_acq_snapshot(const cm_acq *acq, cm_acq_counters *out);

/* Rate windows use serviced DRDY edge counts, including accumulated notifications. */
bool cm_acq_rate_begin(const cm_acq *acq, uint64_t now_us, cm_acq_rate_window *window);
bool cm_acq_rate_finish(cm_acq *acq, const cm_acq_rate_window *window, uint64_t now_us);
uint32_t cm_acq_measured_rate_millihz(const cm_acq *acq);

/*
 * No physical fallback threshold is invented here. AUTO accepts an explicit
 * transport ceiling and an explicit degrade request. It can step down one mode
 * at a time and never silently steps back up inside the same session.
 */
bool cm_acq_policy_initial(cm_acq_policy policy, cm_acq_mode auto_ceiling,
                           cm_acq_mode *selected);
bool cm_acq_policy_step(cm_acq_policy policy, cm_acq_mode current,
                        cm_acq_mode auto_ceiling, bool degrade,
                        cm_acq_mode *selected);

#ifdef __cplusplus
}
#endif
#endif
