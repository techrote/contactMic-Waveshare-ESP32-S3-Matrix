/* Real CM-003 producer, including the fresh snapshot DURING the read callback.
 * Only sensor return values/errors are synthetic. No timing or hardware claim. */
#include "contactmic_acquisition.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct {
    cm_acq core;
    cm_acq_read_result next;
    cm_acq_counters during;
    unsigned calls;
} fixture;
static cm_acq_read_result read_sample(void *context, int16_t *sample)
{
    fixture *f = context;
    CHECK(cm_acq_try_snapshot(&f->core, &f->during));
    CHECK(f->during.drdy_edges - f->during.successful_reads -
        f->during.inferred_missed_intervals == 1);
    ++f->calls;
    *sample = -12345; /* A failed transaction must not publish this value. */
    cm_acq_read_result result = f->next;
    f->next = CM_ACQ_READ_OK;
    return result;
}
static cm_acq_counters completed(fixture *f)
{
    cm_acq_counters c;
    CHECK(cm_acq_try_snapshot(&f->core, &c));
    CHECK(c.drdy_edges == c.successful_reads + c.inferred_missed_intervals);
    CHECK(c.i2c_failures + c.source_faults <= c.inferred_missed_intervals);
    CHECK(c.ring_overruns <= c.successful_reads);
    return c;
}
int main(void)
{
    fixture f = {0};
    CHECK(cm_acq_init(&f.core, read_sample, &f) == CM_ACQ_OK);
    CHECK(cm_acq_start_session(&f.core) == CM_ACQ_OK);
    CHECK(cm_acq_service_edges(&f.core, 3) == CM_ACQ_OK);
    CHECK(f.during.drdy_edges == 3 && f.during.inferred_missed_intervals == 2);
    cm_acq_counters c = completed(&f);
    CHECK(c.successful_reads == 1 && c.inferred_missed_intervals == 2);
    CHECK(cm_acq_consume(&f.core, 1) == 1);
    f.next = CM_ACQ_READ_IO;
    CHECK(cm_acq_service_edges(&f.core, 2) == CM_ACQ_IO);
    CHECK(cm_acq_count(&f.core) == 0);
    c = completed(&f);
    CHECK(c.drdy_edges == 5 && c.inferred_missed_intervals == 4 && c.i2c_failures == 1);
    f.next = CM_ACQ_READ_FAULT;
    CHECK(cm_acq_service_edges(&f.core, 1) == CM_ACQ_SOURCE_FAULT);
    CHECK(cm_acq_count(&f.core) == 0);
    c = completed(&f);
    CHECK(c.source_faults == 1 && c.inferred_missed_intervals == 5);
    CHECK(cm_acq_discard_edges(&f.core, 5) == CM_ACQ_OK);
    CHECK(f.calls == 3); /* Discarded intervals are not diagnostic/acquired reads. */
    c = completed(&f);
    CHECK(c.drdy_edges == 11 && c.inferred_missed_intervals == 10);
    for (unsigned i = 0; i < CM_ACQ_RING_CAPACITY; ++i)
        CHECK(cm_acq_service_edges(&f.core, 1) == CM_ACQ_OK);
    CHECK(cm_acq_service_edges(&f.core, 1) == CM_ACQ_RING_FULL);
    c = completed(&f);
    CHECK(c.successful_reads == 258 && c.ring_overruns == 1);
    CHECK(cm_acq_stop_session(&f.core) == CM_ACQ_OK);
    CHECK(cm_acq_count(&f.core) == 0);
    CHECK(cm_acq_start_session(&f.core) == CM_ACQ_OK);
    cm_acq_counters retained = completed(&f);
    CHECK(retained.drdy_edges == c.drdy_edges && retained.i2c_failures == c.i2c_failures);
    CHECK(retained.source_faults == c.source_faults && retained.ring_overruns == c.ring_overruns);
    CHECK(cm_acq_service_edges(&f.core, 1) == CM_ACQ_OK);
    cm_acq_sample sample;
    CHECK(cm_acq_peek(&f.core, 0, &sample) && sample.index == 0 && sample.value == -12345);
    c = completed(&f);
    CHECK(cm_acq_rebase_publication(&f.core) == CM_ACQ_OK);
    CHECK(cm_acq_count(&f.core) == 0);
    retained = completed(&f);
    CHECK(retained.drdy_edges == c.drdy_edges && retained.successful_reads == c.successful_reads);
    /* A preempted writer is not a fresh snapshot; the API leaves output intact. */
    ++f.core.counter_seq;
    retained.drdy_edges = 123;
    CHECK(!cm_acq_try_snapshot(&f.core, &retained) && retained.drdy_edges == 123);
    ++f.core.counter_seq;
    (void)completed(&f);
    puts("CM-009 real source ledger: in-flight, errors, loss, bounds and lifecycle passed");
    return 0;
}
