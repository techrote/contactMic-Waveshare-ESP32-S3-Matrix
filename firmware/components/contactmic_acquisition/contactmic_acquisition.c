#include "contactmic_acquisition.h"
#include <limits.h>
#include <string.h>

#define LOAD32(p, order) __atomic_load_n((p), (order))
#define STORE32(p, v, order) __atomic_store_n((p), (v), (order))

_Static_assert((CM_ACQ_RING_CAPACITY & (CM_ACQ_RING_CAPACITY - 1u)) == 0u,
               "CM_ACQ_RING_CAPACITY must be a power of two");

static uint64_t sat_add(uint64_t value, uint64_t increment)
{
    return UINT64_MAX - value < increment ? UINT64_MAX : value + increment;
}

static uint64_t counter_load(const cm_acq_counter64_storage *counter)
{
    uint32_t low = LOAD32(&counter->low, __ATOMIC_RELAXED);
    uint32_t high = LOAD32(&counter->high, __ATOMIC_RELAXED);
    return ((uint64_t)high << 32) | low;
}

static void counter_store(cm_acq_counter64_storage *counter, uint64_t value)
{
    STORE32(&counter->low, (uint32_t)value, __ATOMIC_RELAXED);
    STORE32(&counter->high, (uint32_t)(value >> 32), __ATOMIC_RELAXED);
}

static void counter_add(cm_acq_counter64_storage *counter, uint64_t increment)
{
    counter_store(counter, sat_add(counter_load(counter), increment));
}

static void counters_begin(cm_acq *acq)
{
    (void)__atomic_add_fetch(&acq->counter_seq, 1u, __ATOMIC_ACQ_REL);
}

static void counters_end(cm_acq *acq)
{
    (void)__atomic_add_fetch(&acq->counter_seq, 1u, __ATOMIC_RELEASE);
}

static void ring_clear(cm_acq *acq)
{
    STORE32(&acq->ring.tail, 0u, __ATOMIC_RELEASE);
    STORE32(&acq->ring.head, 0u, __ATOMIC_RELEASE);
}

static bool ring_push(cm_acq *acq, cm_acq_sample sample)
{
    uint32_t head = LOAD32(&acq->ring.head, __ATOMIC_RELAXED);
    uint32_t tail = LOAD32(&acq->ring.tail, __ATOMIC_ACQUIRE);
    if ((uint32_t)(head - tail) >= CM_ACQ_RING_CAPACITY) return false;
    acq->ring.slots[head & (CM_ACQ_RING_CAPACITY - 1u)] = sample;
    STORE32(&acq->ring.head, head + 1u, __ATOMIC_RELEASE);
    return true;
}

static bool valid_mode(cm_acq_mode mode)
{
    return mode >= CM_ACQ_MODE_SAFE && mode <= CM_ACQ_MODE_HIGH;
}

static bool valid_policy(cm_acq_policy policy)
{
    return policy >= CM_ACQ_POLICY_SAFE && policy <= CM_ACQ_POLICY_AUTO;
}

cm_acq_result cm_acq_init(cm_acq *acq, cm_acq_read_fn read, void *read_context)
{
    if (!acq || !read) return CM_ACQ_INVALID;
    memset(acq, 0, sizeof(*acq));
    acq->read = read;
    acq->read_context = read_context;
    return CM_ACQ_OK;
}

cm_acq_result cm_acq_start_session(cm_acq *acq)
{
    if (!acq || !acq->read) return CM_ACQ_INVALID;
    if (LOAD32(&acq->running, __ATOMIC_ACQUIRE)) return CM_ACQ_STATE;
    ring_clear(acq);
    acq->next_index = 0u;
    STORE32(&acq->measured_rate_millihz, 0u, __ATOMIC_RELEASE);
    STORE32(&acq->running, 1u, __ATOMIC_RELEASE);
    return CM_ACQ_OK;
}

cm_acq_result cm_acq_stop_session(cm_acq *acq)
{
    if (!acq) return CM_ACQ_INVALID;
    STORE32(&acq->running, 0u, __ATOMIC_RELEASE);
    ring_clear(acq);
    return CM_ACQ_OK;
}

cm_acq_result cm_acq_service_edges(cm_acq *acq, uint32_t edges)
{
    if (!acq || !acq->read || edges == 0u) return CM_ACQ_INVALID;
    if (!LOAD32(&acq->running, __ATOMIC_ACQUIRE)) return CM_ACQ_STATE;

    uint32_t sample_index = acq->next_index + (edges - 1u);
    acq->next_index += edges;

    counters_begin(acq);
    counter_add(&acq->counter_store.drdy_edges, edges);
    if (edges > 1u) {
        counter_add(&acq->counter_store.inferred_missed_intervals, (uint64_t)edges - 1u);
    }
    counters_end(acq);

    int16_t value = 0;
    cm_acq_read_result read_result = acq->read(acq->read_context, &value);
    if (read_result != CM_ACQ_READ_OK) {
        counters_begin(acq);
        counter_add(&acq->counter_store.inferred_missed_intervals, 1u);
        if (read_result == CM_ACQ_READ_IO) {
            counter_add(&acq->counter_store.i2c_failures, 1u);
        } else {
            counter_add(&acq->counter_store.source_faults, 1u);
        }
        counters_end(acq);
        return read_result == CM_ACQ_READ_IO ? CM_ACQ_IO : CM_ACQ_SOURCE_FAULT;
    }

    counters_begin(acq);
    counter_add(&acq->counter_store.successful_reads, 1u);
    counters_end(acq);

    cm_acq_sample sample = {sample_index, value};
    if (!ring_push(acq, sample)) {
        counters_begin(acq);
        counter_add(&acq->counter_store.ring_overruns, 1u);
        counters_end(acq);
        return CM_ACQ_RING_FULL;
    }
    return CM_ACQ_OK;
}

size_t cm_acq_count(const cm_acq *acq)
{
    if (!acq) return 0u;
    uint32_t head = LOAD32(&acq->ring.head, __ATOMIC_ACQUIRE);
    uint32_t tail = LOAD32(&acq->ring.tail, __ATOMIC_ACQUIRE);
    uint32_t count = head - tail;
    return count > CM_ACQ_RING_CAPACITY ? CM_ACQ_RING_CAPACITY : (size_t)count;
}

bool cm_acq_peek(const cm_acq *acq, size_t offset, cm_acq_sample *sample)
{
    if (!acq || !sample || offset >= CM_ACQ_RING_CAPACITY) return false;
    uint32_t tail = LOAD32(&acq->ring.tail, __ATOMIC_ACQUIRE);
    uint32_t head = LOAD32(&acq->ring.head, __ATOMIC_ACQUIRE);
    uint32_t available = head - tail;
    if (offset >= available || available > CM_ACQ_RING_CAPACITY) return false;
    *sample = acq->ring.slots[(tail + (uint32_t)offset) & (CM_ACQ_RING_CAPACITY - 1u)];
    return true;
}

size_t cm_acq_consume(cm_acq *acq, size_t count)
{
    if (!acq || count == 0u) return 0u;
    uint32_t tail = LOAD32(&acq->ring.tail, __ATOMIC_RELAXED);
    uint32_t head = LOAD32(&acq->ring.head, __ATOMIC_ACQUIRE);
    uint32_t available = head - tail;
    if (available > CM_ACQ_RING_CAPACITY) return 0u;
    uint32_t take = count < available ? (uint32_t)count : available;
    STORE32(&acq->ring.tail, tail + take, __ATOMIC_RELEASE);
    return take;
}

void cm_acq_snapshot(const cm_acq *acq, cm_acq_counters *out)
{
    if (!out) return;
    if (!acq) {
        memset(out, 0, sizeof(*out));
        return;
    }
    for (;;) {
        uint32_t before = LOAD32(&acq->counter_seq, __ATOMIC_ACQUIRE);
        if (before & 1u) continue;
        cm_acq_counters copy = {
            .drdy_edges = counter_load(&acq->counter_store.drdy_edges),
            .successful_reads = counter_load(&acq->counter_store.successful_reads),
            .inferred_missed_intervals =
                counter_load(&acq->counter_store.inferred_missed_intervals),
            .i2c_failures = counter_load(&acq->counter_store.i2c_failures),
            .source_faults = counter_load(&acq->counter_store.source_faults),
            .ring_overruns = counter_load(&acq->counter_store.ring_overruns),
        };
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t after = LOAD32(&acq->counter_seq, __ATOMIC_ACQUIRE);
        if (before == after && !(after & 1u)) {
            *out = copy;
            return;
        }
    }
}

bool cm_acq_rate_begin(const cm_acq *acq, uint64_t now_us, cm_acq_rate_window *window)
{
    if (!acq || !window || !LOAD32(&acq->running, __ATOMIC_ACQUIRE)) return false;
    cm_acq_counters counters;
    cm_acq_snapshot(acq, &counters);
    window->start_us = now_us;
    window->start_edges = counters.drdy_edges;
    return true;
}

bool cm_acq_rate_finish(cm_acq *acq, const cm_acq_rate_window *window, uint64_t now_us)
{
    if (!acq || !window || now_us <= window->start_us) return false;
    cm_acq_counters counters;
    cm_acq_snapshot(acq, &counters);
    if (counters.drdy_edges < window->start_edges ||
        counters.drdy_edges == UINT64_MAX) return false;

    uint64_t edges = counters.drdy_edges - window->start_edges;
    uint64_t elapsed_us = now_us - window->start_us;
    uint64_t rate;
    if (edges > UINT64_MAX / 1000000000ull) {
        rate = UINT32_MAX;
    } else {
        rate = (edges * 1000000000ull + elapsed_us / 2u) / elapsed_us;
        if (rate > UINT32_MAX) rate = UINT32_MAX;
    }
    STORE32(&acq->measured_rate_millihz, (uint32_t)rate, __ATOMIC_RELEASE);
    return true;
}

uint32_t cm_acq_measured_rate_millihz(const cm_acq *acq)
{
    return acq ? LOAD32(&acq->measured_rate_millihz, __ATOMIC_ACQUIRE) : 0u;
}

bool cm_acq_policy_initial(cm_acq_policy policy, cm_acq_mode ceiling,
                           cm_acq_mode *selected)
{
    if (!selected || !valid_policy(policy) || !valid_mode(ceiling)) return false;
    if (policy == CM_ACQ_POLICY_AUTO) {
        *selected = ceiling;
    } else {
        *selected = (cm_acq_mode)policy;
    }
    return true;
}

bool cm_acq_policy_step(cm_acq_policy policy, cm_acq_mode current,
                        cm_acq_mode ceiling, bool degrade,
                        cm_acq_mode *selected)
{
    if (!selected || !valid_policy(policy) || !valid_mode(current) ||
        !valid_mode(ceiling)) return false;
    if (policy != CM_ACQ_POLICY_AUTO) {
        *selected = (cm_acq_mode)policy;
        return true;
    }

    cm_acq_mode next = current > ceiling ? ceiling : current;
    if (degrade && next > CM_ACQ_MODE_SAFE) {
        next = (cm_acq_mode)(next - 1);
    }
    *selected = next;
    return true;
}
