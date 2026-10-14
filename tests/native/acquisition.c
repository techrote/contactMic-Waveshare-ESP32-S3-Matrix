#include "contactmic_acquisition.h"
#include <stdint.h>
#include <stdio.h>

#define CHECK(expr) do { if (!(expr)) {     fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #expr);     return __LINE__; } } while (0)

typedef struct {
    int32_t value;
    cm_acq_read_result next_result;
} fake_reader;

static cm_acq_read_result read_sample(void *opaque, int16_t *sample)
{
    fake_reader *reader = (fake_reader *)opaque;
    cm_acq_read_result result = reader->next_result;
    reader->next_result = CM_ACQ_READ_OK;
    if (result != CM_ACQ_READ_OK) return result;
    *sample = (int16_t)(uint16_t)reader->value++;
    return CM_ACQ_READ_OK;
}

static int test_ring_and_overflow(void)
{
    fake_reader reader = {0};
    cm_acq acq;
    CHECK(cm_acq_init(&acq, read_sample, &reader) == CM_ACQ_OK);
    CHECK(cm_acq_start_session(&acq) == CM_ACQ_OK);

    for (uint32_t i = 0; i < CM_ACQ_RING_CAPACITY; ++i)
        CHECK(cm_acq_service_edges(&acq, 1) == CM_ACQ_OK);
    CHECK(cm_acq_count(&acq) == CM_ACQ_RING_CAPACITY);

    cm_acq_sample sample;
    CHECK(cm_acq_peek(&acq, 0, &sample) && sample.index == 0 && sample.value == 0);
    CHECK(cm_acq_peek(&acq, CM_ACQ_RING_CAPACITY - 1u, &sample));
    CHECK(sample.index == CM_ACQ_RING_CAPACITY - 1u);

    CHECK(cm_acq_service_edges(&acq, 1) == CM_ACQ_RING_FULL);
    CHECK(cm_acq_count(&acq) == CM_ACQ_RING_CAPACITY);
    cm_acq_counters counters;
    cm_acq_snapshot(&acq, &counters);
    CHECK(counters.successful_reads == CM_ACQ_RING_CAPACITY + 1u);
    CHECK(counters.ring_overruns == 1u);
    CHECK(counters.inferred_missed_intervals == 0u);

    CHECK(cm_acq_consume(&acq, CM_ACQ_RING_CAPACITY) == CM_ACQ_RING_CAPACITY);
    CHECK(cm_acq_count(&acq) == 0u);
    CHECK(cm_acq_service_edges(&acq, 1) == CM_ACQ_OK);
    CHECK(cm_acq_peek(&acq, 0, &sample));
    CHECK(sample.index == CM_ACQ_RING_CAPACITY + 1u);
    return 0;
}

static int test_accumulated_edges_and_failures(void)
{
    fake_reader reader = {.value = 10, .next_result = CM_ACQ_READ_OK};
    cm_acq acq;
    CHECK(cm_acq_init(&acq, read_sample, &reader) == CM_ACQ_OK);
    CHECK(cm_acq_start_session(&acq) == CM_ACQ_OK);

    CHECK(cm_acq_service_edges(&acq, 3) == CM_ACQ_OK);
    cm_acq_sample sample;
    CHECK(cm_acq_peek(&acq, 0, &sample));
    CHECK(sample.index == 2u && sample.value == 10);
    CHECK(cm_acq_consume(&acq, 1) == 1u);

    reader.next_result = CM_ACQ_READ_IO;
    CHECK(cm_acq_service_edges(&acq, 2) == CM_ACQ_IO);
    CHECK(cm_acq_count(&acq) == 0u);
    CHECK(cm_acq_service_edges(&acq, 1) == CM_ACQ_OK);
    CHECK(cm_acq_peek(&acq, 0, &sample) && sample.index == 5u);

    cm_acq_counters counters;
    cm_acq_snapshot(&acq, &counters);
    CHECK(counters.drdy_edges == 6u);
    CHECK(counters.successful_reads == 2u);
    CHECK(counters.inferred_missed_intervals == 4u);
    CHECK(counters.i2c_failures == 1u);
    CHECK(counters.source_faults == 0u);

    reader.next_result = CM_ACQ_READ_FAULT;
    CHECK(cm_acq_service_edges(&acq, 1) == CM_ACQ_SOURCE_FAULT);
    cm_acq_snapshot(&acq, &counters);
    CHECK(counters.source_faults == 1u);
    CHECK(counters.inferred_missed_intervals == 5u);
    return 0;
}

static int test_lifecycle_clears_stale_data(void)
{
    fake_reader reader = {0};
    cm_acq acq;
    cm_acq_sample sample;
    CHECK(cm_acq_init(&acq, read_sample, &reader) == CM_ACQ_OK);
    CHECK(cm_acq_start_session(&acq) == CM_ACQ_OK);
    CHECK(cm_acq_service_edges(&acq, 1) == CM_ACQ_OK);
    CHECK(cm_acq_count(&acq) == 1u);
    CHECK(cm_acq_stop_session(&acq) == CM_ACQ_OK);
    CHECK(cm_acq_count(&acq) == 0u);
    CHECK(cm_acq_service_edges(&acq, 1) == CM_ACQ_STATE);
    CHECK(cm_acq_start_session(&acq) == CM_ACQ_OK);
    CHECK(cm_acq_service_edges(&acq, 1) == CM_ACQ_OK);
    CHECK(cm_acq_peek(&acq, 0, &sample));
    CHECK(sample.index == 0u);
    return 0;
}

static int test_index_wrap_and_saturation(void)
{
    fake_reader reader = {0};
    cm_acq acq;
    cm_acq_sample sample;
    CHECK(cm_acq_init(&acq, read_sample, &reader) == CM_ACQ_OK);
    CHECK(cm_acq_start_session(&acq) == CM_ACQ_OK);

    CHECK(cm_acq_service_edges(&acq, UINT64_MAX) == CM_ACQ_OK);
    CHECK(cm_acq_peek(&acq, 0, &sample));
    CHECK(sample.index == UINT32_MAX - 1u);
    CHECK(cm_acq_consume(&acq, 1) == 1u);

    cm_acq_counters counters;
    cm_acq_snapshot(&acq, &counters);
    CHECK(counters.drdy_edges == UINT64_MAX);
    CHECK(counters.inferred_missed_intervals == UINT64_MAX - 1u);

    reader.next_result = CM_ACQ_READ_IO;
    CHECK(cm_acq_service_edges(&acq, 2u) == CM_ACQ_IO);
    CHECK(cm_acq_peek(&acq, 0, &sample) == false);
    cm_acq_snapshot(&acq, &counters);
    CHECK(counters.drdy_edges == UINT64_MAX);
    CHECK(counters.inferred_missed_intervals == UINT64_MAX);
    CHECK(counters.i2c_failures == 1u);
    return 0;
}

static int test_discarded_pending_edges(void)
{
    fake_reader reader = {0};
    cm_acq acq;
    cm_acq_sample sample;
    CHECK(cm_acq_init(&acq, read_sample, &reader) == CM_ACQ_OK);
    CHECK(cm_acq_start_session(&acq) == CM_ACQ_OK);
    CHECK(cm_acq_service_edges(&acq, 1u) == CM_ACQ_OK);
    CHECK(cm_acq_consume(&acq, 1u) == 1u);

    CHECK(cm_acq_discard_edges(&acq, 3u) == CM_ACQ_OK);
    CHECK(cm_acq_count(&acq) == 0u);
    CHECK(cm_acq_service_edges(&acq, 1u) == CM_ACQ_OK);
    CHECK(cm_acq_peek(&acq, 0u, &sample));
    CHECK(sample.index == 4u);

    cm_acq_counters counters;
    cm_acq_snapshot(&acq, &counters);
    CHECK(counters.drdy_edges == 5u);
    CHECK(counters.successful_reads == 2u);
    CHECK(counters.inferred_missed_intervals == 3u);
    CHECK(cm_acq_discard_edges(&acq, 0u) == CM_ACQ_INVALID);
    return 0;
}

static int test_rate_measurement(void)
{
    fake_reader reader = {0};
    cm_acq acq;
    cm_acq_rate_window window;
    CHECK(cm_acq_init(&acq, read_sample, &reader) == CM_ACQ_OK);
    CHECK(cm_acq_start_session(&acq) == CM_ACQ_OK);
    CHECK(cm_acq_rate_begin(&acq, 1000000u, &window));
    CHECK(cm_acq_service_edges(&acq, 180u) == CM_ACQ_OK);
    CHECK(cm_acq_rate_finish(&acq, &window, 1200000u));
    CHECK(cm_acq_measured_rate_millihz(&acq) == 900000u);

    CHECK(cm_acq_stop_session(&acq) == CM_ACQ_OK);
    CHECK(cm_acq_start_session(&acq) == CM_ACQ_OK);
    CHECK(cm_acq_rate_begin(&acq, 2000000u, &window));
    CHECK(cm_acq_rate_finish(&acq, &window, 2100000u));
    CHECK(cm_acq_measured_rate_millihz(&acq) == 0u);
    CHECK(!cm_acq_rate_finish(&acq, &window, 2000000u));
    return 0;
}

static int test_policy_state_machine(void)
{
    cm_acq_mode selected;
    CHECK(cm_acq_policy_initial(CM_ACQ_POLICY_HIGH, CM_ACQ_MODE_SAFE, &selected));
    CHECK(selected == CM_ACQ_MODE_HIGH);
    CHECK(cm_acq_policy_initial(CM_ACQ_POLICY_AUTO, CM_ACQ_MODE_HIGH, &selected));
    CHECK(selected == CM_ACQ_MODE_HIGH);

    CHECK(cm_acq_policy_step(CM_ACQ_POLICY_AUTO, CM_ACQ_MODE_HIGH,
                             CM_ACQ_MODE_HIGH, true, &selected));
    CHECK(selected == CM_ACQ_MODE_MEDIUM);
    CHECK(cm_acq_policy_step(CM_ACQ_POLICY_AUTO, CM_ACQ_MODE_MEDIUM,
                             CM_ACQ_MODE_HIGH, true, &selected));
    CHECK(selected == CM_ACQ_MODE_SAFE);
    CHECK(cm_acq_policy_step(CM_ACQ_POLICY_AUTO, CM_ACQ_MODE_SAFE,
                             CM_ACQ_MODE_HIGH, true, &selected));
    CHECK(selected == CM_ACQ_MODE_SAFE);

    CHECK(cm_acq_policy_step(CM_ACQ_POLICY_AUTO, CM_ACQ_MODE_HIGH,
                             CM_ACQ_MODE_MEDIUM, false, &selected));
    CHECK(selected == CM_ACQ_MODE_MEDIUM);
    CHECK(cm_acq_policy_step(CM_ACQ_POLICY_AUTO, CM_ACQ_MODE_MEDIUM,
                             CM_ACQ_MODE_HIGH, false, &selected));
    CHECK(selected == CM_ACQ_MODE_MEDIUM);
    CHECK(cm_acq_policy_step(CM_ACQ_POLICY_SAFE, CM_ACQ_MODE_HIGH,
                             CM_ACQ_MODE_HIGH, false, &selected));
    CHECK(selected == CM_ACQ_MODE_SAFE);
    CHECK(!cm_acq_policy_initial((cm_acq_policy)99, CM_ACQ_MODE_HIGH, &selected));
    return 0;
}

static int test_stress(void)
{
    fake_reader reader = {0};
    cm_acq acq;
    CHECK(cm_acq_init(&acq, read_sample, &reader) == CM_ACQ_OK);
    CHECK(cm_acq_start_session(&acq) == CM_ACQ_OK);

    uint32_t model_next = 0u;
    for (uint32_t i = 0; i < 100000u; ++i) {
        uint32_t edges = (i % 97u == 0u) ? 3u : 1u;
        uint32_t expected = model_next + edges - 1u;
        cm_acq_result result = cm_acq_service_edges(&acq, edges);
        CHECK(result == CM_ACQ_OK);
        model_next += edges;

        cm_acq_sample sample;
        CHECK(cm_acq_count(&acq) == 1u);
        CHECK(cm_acq_peek(&acq, 0, &sample));
        CHECK(sample.index == expected);
        CHECK(cm_acq_consume(&acq, 1) == 1u);
    }
    CHECK(cm_acq_count(&acq) == 0u);
    return 0;
}

int main(void)
{
    int rc;
    if ((rc = test_ring_and_overflow())) return rc;
    if ((rc = test_accumulated_edges_and_failures())) return rc;
    if ((rc = test_lifecycle_clears_stale_data())) return rc;
    if ((rc = test_index_wrap_and_saturation())) return rc;
    if ((rc = test_discarded_pending_edges())) return rc;
    if ((rc = test_rate_measurement())) return rc;
    if ((rc = test_policy_state_machine())) return rc;
    if ((rc = test_stress())) return rc;
    return 0;
}
