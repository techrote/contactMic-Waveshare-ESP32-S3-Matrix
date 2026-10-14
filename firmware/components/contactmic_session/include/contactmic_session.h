#ifndef CONTACTMIC_SESSION_H
#define CONTACTMIC_SESSION_H
#include "contactmic_acquisition.h"
#include "contactmic_ble.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Only worker_step calls these lifecycle functions. They may perform bounded
 * sensor I/O. ready returns BUSY during calibration; publish requests a fresh
 * producer-owned index origin; published acknowledges that origin. */
typedef struct {
    void *context;
    cm_result (*begin)(void *, const cm_config *, uint8_t selected_mode);
    cm_result (*ready)(void *, uint32_t *measured_rate);
    cm_result (*publish)(void *);
    bool (*published)(void *);
    cm_result (*stop)(void *);
} cm_session_backend;

enum { CMS_IDLE, CMS_MEASURING, CMS_READY, CMS_LAUNCHING, CMS_LIVE, CMS_FAILED };
enum { CMS_STOP, CMS_PREPARE, CMS_COMMIT };
enum { CMS_REASON_SOURCE=1, CMS_REASON_I2C=2, CMS_REASON_RING=4,
       CMS_REASON_NOTIFY=8, CMS_REASON_CAPACITY=16, CMS_REASON_BACKEND=32 };
/* Every shared scalar uses 32-bit atomics. No locks or unbounded read retries. */
typedef struct { uint32_t sequence, generation, kind, config, mode; } cm_session_command;
typedef struct { uint32_t sequence, generation, state, rate, result; } cm_session_reply;
typedef struct {
    cm_acq *acquisition;
    cm_session_backend backend;
    cm_session_command command;
    cm_session_reply reply;
    /* Frontend/NimBLE-owner fields. */
    uint32_t generation, request_config, capacity_millihz;
    uint8_t request_mode, auto_ceiling;
    bool active, visible, prepared, monitoring;
    cm_acq_counters cached;
    cm_source_counters health_base;
    uint64_t notify_base, monitor_us, transitions;
    uint32_t monitored_session, last_reason;
    /* Worker-only fields. Do not read them from the frontend. */
    uint32_t worker_generation, worker_state, worker_kind;
    uint64_t worker_started_us;
} cm_session;

cm_result cm_session_init(cm_session *, cm_acq *, const cm_session_backend *, uint8_t auto_ceiling);
cm_source cm_session_source(cm_session *);
/* Dedicated controller task only; one bounded state-machine step per call.
 * Stop/new-prepare can supersede a blocked prior operation. Replies are tagged,
 * and stale operation completion can never reopen frontend publication. */
void cm_session_worker_step(cm_session *, uint64_t now_us);
/* NimBLE owner only, before control handling and packet pumping. The scheduling
 * budget is an upper bound, NOT measured radio throughput. Health is sampled at
 * one-second intervals; any new loss in AUTO requests a conservative downshift.
 * Return true on an explicit stop/fallback boundary, for diagnostics. */
bool cm_session_monitor(cm_session *, cm_transport *, uint64_t now_us,
                        uint32_t notification_budget_per_second);
uint32_t cm_session_capacity(uint16_t mtu, uint32_t notification_budget_per_second);
#ifdef __cplusplus
}
#endif
#endif
