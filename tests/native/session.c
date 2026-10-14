#include "contactmic_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
typedef struct {
    cm_acq acq; cm_session session; cm_source source; cm_transport transport;
    uint32_t rate; unsigned begins, stops, publishes, reads;
    bool ready, live, cancel_in_begin, stop_fail;
    cm_acq_read_result next_read;
    unsigned notifications; bool reject;
    cm_audio last;
} fixture;
static cm_acq_read_result sample_read(void *p, int16_t *v)
{
    fixture *f=p; ++f->reads; *v=(int16_t)(f->reads % 30000);
    cm_acq_read_result result=f->next_read; f->next_read=CM_ACQ_READ_OK; return result;
}
static cm_result begin(void *p, const cm_config *c, uint8_t mode)
{
    fixture *f=p; CHECK(cm_config_valid(c)); CHECK(mode <= CM_POLICY_HIGH);
    ++f->begins; f->live=false;
    CHECK(cm_acq_start_session(&f->acq)==CM_ACQ_OK);
    if (f->cancel_in_begin) { f->cancel_in_begin=false; f->source.stop(&f->session); }
    return CM_OK;
}
static cm_result ready(void *p, uint32_t *rate)
{ fixture *f=p; if (!f->ready) return CM_BUSY; *rate=f->rate; return CM_OK; }
static cm_result publish(void *p)
{
    fixture *f=p; ++f->publishes;
    CHECK(cm_acq_rebase_publication(&f->acq)==CM_ACQ_OK); f->live=true; return CM_OK;
}
static bool published(void *p) { return ((fixture *)p)->live; }
static cm_result stop(void *p)
{
    fixture *f=p; ++f->stops; f->live=false;
    CHECK(cm_acq_stop_session(&f->acq)==CM_ACQ_OK);
    return f->stop_fail ? CM_IO : CM_OK;
}
static bool notify(void *p, const uint8_t *bytes, size_t n)
{
    fixture *f=p; CHECK(cm_audio_decode(bytes,n,&f->last)==CM_OK);
    ++f->notifications; return !f->reject;
}
static cm_result control(fixture *f, uint8_t op, uint8_t value)
{
    const cm_control c={.opcode=op,.value=value,.session_id=f->transport.status.session_id};
    return cm_transport_control(&f->transport,&c);
}
static void init(fixture *f)
{
    memset(f,0,sizeof(*f)); f->ready=true; f->rate=1000000;
    CHECK(cm_acq_init(&f->acq,sample_read,f)==CM_ACQ_OK);
    cm_session_backend b={f,begin,ready,publish,published,stop};
    CHECK(cm_session_init(&f->session,&f->acq,&b,CM_POLICY_HIGH)==CM_OK);
    f->source=cm_session_source(&f->session);
    CHECK(cm_transport_init(&f->transport,&f->source,12)==CM_OK);
    cm_transport_connect(&f->transport); cm_transport_mtu(&f->transport,247);
    CHECK(!cm_session_monitor(&f->session,&f->transport,0,400));
}
static void prepare(fixture *f, uint8_t policy)
{
    CHECK(control(f,CM_OP_SET_POLICY,policy)==CM_OK);
    unsigned begins=f->begins, stops=f->stops;
    CHECK(control(f,CM_OP_START,0)==CM_BUSY);
    CHECK(f->begins==begins && f->stops==stops); /* GATT did no backend work. */
    cm_session_worker_step(&f->session,0);
    cm_session_worker_step(&f->session,250000);
    CHECK(control(f,CM_OP_START,0)==CM_OK);
    CHECK(f->transport.status.state==CM_PREPARED);
}
static void commit(fixture *f)
{
    cm_transport_subscribe(&f->transport,true);
    unsigned n=f->publishes;
    CHECK(control(f,CM_OP_COMMIT_START,0)==CM_OK);
    CHECK(f->publishes==n);
    CHECK(cm_transport_pump(&f->transport,notify,f)==CM_BUSY);
    cm_session_worker_step(&f->session,260000);
    CHECK(f->publishes==n+1);
    CHECK(!cm_session_monitor(&f->session,&f->transport,300000,400));
}
static void restart(fixture *f)
{
    cm_transport_disconnect(&f->transport);
    cm_transport_connect(&f->transport); cm_transport_mtu(&f->transport,247);
    CHECK(!cm_session_monitor(&f->session,&f->transport,0,400));
}
static void test_pipeline_faults(void)
{
    fixture f; init(&f); prepare(&f,CM_POLICY_HIGH);
    /* Calibration is not the stream's origin. */
    CHECK(cm_acq_service_edges(&f.acq,20)==CM_ACQ_OK);
    commit(&f); CHECK(cm_acq_count(&f.acq)==0);
    CHECK(cm_acq_service_edges(&f.acq,1)==CM_ACQ_OK);
    CHECK(cm_transport_pump(&f.transport,notify,&f)==CM_OK);
    CHECK(f.last.first_index==0 && f.last.rate_millihz==f.rate);
    CHECK(cm_acq_service_edges(&f.acq,3)==CM_ACQ_OK);
    CHECK(cm_transport_pump(&f.transport,notify,&f)==CM_OK);
    CHECK(f.last.first_index==3 && (f.last.flags & CM_FLAG_SOURCE_GAP));
    f.next_read=CM_ACQ_READ_IO;
    CHECK(cm_acq_service_edges(&f.acq,1)==CM_ACQ_IO);
    CHECK(cm_acq_service_edges(&f.acq,1)==CM_ACQ_OK);
    CHECK(cm_transport_pump(&f.transport,notify,&f)==CM_OK);
    CHECK(f.last.first_index==5 && (f.last.flags & CM_FLAG_I2C_ERROR));
    for (unsigned i=0;i<CM_ACQ_RING_CAPACITY;i++) CHECK(cm_acq_service_edges(&f.acq,1)==CM_ACQ_OK);
    CHECK(cm_acq_service_edges(&f.acq,1)==CM_ACQ_RING_FULL);
    f.reject=true; CHECK(cm_transport_pump(&f.transport,notify,&f)==CM_BUSY);
    f.reject=false; CHECK(cm_transport_pump(&f.transport,notify,&f)==CM_OK);
    CHECK(f.last.flags & CM_FLAG_RING_OVERRUN);
    CHECK(f.transport.status.notify_rejections==1);
    CHECK(f.transport.status.i2c_errors==1 && f.transport.status.ring_overruns==1);
    unsigned stops=f.stops; uint64_t misses=f.transport.status.source_misses;
    CHECK(control(&f,CM_OP_STOP,0)==CM_OK); CHECK(f.stops==stops);
    cm_source_sample x; CHECK(!f.source.peek(&f.session,0,&x));
    CHECK(control(&f,CM_OP_START,0)==CM_STATE); /* v1 committed connection cannot restart */
    restart(&f); prepare(&f,CM_POLICY_MEDIUM); commit(&f);
    CHECK(cm_acq_service_edges(&f.acq,1)==CM_ACQ_OK);
    CHECK(cm_transport_pump(&f.transport,notify,&f)==CM_OK);
    CHECK(f.last.first_index==0 && f.last.sequence==0);
    CHECK(f.transport.status.source_misses==misses && f.transport.status.i2c_errors==1);
}
static void test_cancel_and_mailboxes(void)
{
    fixture f; init(&f); f.cancel_in_begin=true;
    CHECK(control(&f,CM_OP_START,0)==CM_BUSY);
    /* Backend calls source.stop with its correct frontend context. */
    cm_session_worker_step(&f.session,0); /* begin in progress in the real scheduler */
    CHECK(!f.session.active); /* begin was superseded before it returned. */
    cm_session_worker_step(&f.session,250000);
    cm_source_sample x; CHECK(!f.source.peek(&f.session,0,&x));
    CHECK(control(&f,CM_OP_START,0)==CM_BUSY);
    cm_session_worker_step(&f.session,300000);
    cm_session_worker_step(&f.session,550000);
    CHECK(control(&f,CM_OP_START,0)==CM_OK); commit(&f);
    /* A preempted diagnostics writer cannot stall the BLE owner. */
    cm_source_counters before,after; f.source.counters(&f.session,&before);
    __atomic_store_n(&f.acq.counter_seq,1,__ATOMIC_SEQ_CST);
    f.source.counters(&f.session,&after);
    CHECK(before.missed==after.missed && before.i2c_errors==after.i2c_errors);
    __atomic_store_n(&f.acq.counter_seq,2,__ATOMIC_SEQ_CST);
    /* Superseded request's READY reply is not current. */
    f.source.stop(&f.session);
    CHECK(control(&f,CM_OP_STOP,0)==CM_OK); restart(&f);
    CHECK(control(&f,CM_OP_START,0)==CM_BUSY);
    __atomic_store_n(&f.session.reply.state,CMS_READY,__ATOMIC_SEQ_CST);
    CHECK(control(&f,CM_OP_START,0)==CM_BUSY);
    (void)x;
}
static void test_capacity_auto_and_errors(void)
{
    fixture f; init(&f);
    cm_transport_mtu(&f.transport,23);
    CHECK(!cm_session_monitor(&f.session,&f.transport,0,400));
    CHECK(control(&f,CM_OP_START,0)==CM_CAPACITY && f.begins==0);
    cm_transport_mtu(&f.transport,247);
    CHECK(!cm_session_monitor(&f.session,&f.transport,0,400));
    prepare(&f,CM_POLICY_AUTO); CHECK(f.transport.status.selected_mode==CM_POLICY_HIGH); commit(&f);
    f.next_read=CM_ACQ_READ_IO; CHECK(cm_acq_service_edges(&f.acq,1)==CM_ACQ_IO);
    CHECK(cm_session_monitor(&f.session,&f.transport,1300000,400));
    CHECK(f.transport.status.state==CM_STOPPED && f.transport.status.selected_mode==CM_POLICY_MEDIUM);
    CHECK(f.session.last_reason & CMS_REASON_I2C);
    restart(&f); prepare(&f,CM_POLICY_AUTO); CHECK(f.transport.status.selected_mode==CM_POLICY_MEDIUM); commit(&f);
    cm_transport_notify_failed(&f.transport);
    CHECK(cm_session_monitor(&f.session,&f.transport,1300000,400));
    CHECK(f.session.auto_ceiling==CM_POLICY_SAFE);
    CHECK(f.transport.status.i2c_errors==1 && f.transport.status.notify_rejections==1);
    restart(&f); prepare(&f,CM_POLICY_AUTO); CHECK(f.transport.status.selected_mode==CM_POLICY_SAFE); commit(&f);
    cm_transport_mtu(&f.transport,23);
    CHECK(cm_session_monitor(&f.session,&f.transport,300001,400));
    CHECK(f.transport.status.state==CM_STOPPED);
    CHECK(f.session.auto_ceiling==CM_POLICY_SAFE);
    /* A stopped hardware failure is attempted once, not blindly every tick. */
    f.stop_fail=true;
    cm_session_worker_step(&f.session,1400000);
    unsigned count=f.stops;
    for(unsigned i=0;i<10;i++) cm_session_worker_step(&f.session,1400001+i);
    CHECK(f.stops==count);
}
static void test_bounds(void)
{
    fixture f; init(&f); f.ready=false;
    CHECK(control(&f,CM_OP_START,0)==CM_BUSY); cm_session_worker_step(&f.session,0);
    CHECK(control(&f,CM_OP_SET_AXIS,CM_AXIS_X)==CM_STATE);
    cm_session_worker_step(&f.session,2000000);
    CHECK(control(&f,CM_OP_START,0)==CM_IO);
    f.ready=true; f.rate=0;
    CHECK(control(&f,CM_OP_START,0)==CM_BUSY); cm_session_worker_step(&f.session,0); cm_session_worker_step(&f.session,1);
    CHECK(control(&f,CM_OP_START,0)==CM_IO);
    f.session.generation=UINT32_MAX;
    CHECK(control(&f,CM_OP_START,0)==CM_STATE);
    CHECK(control(&f,CM_OP_STOP,0)==CM_OK);
    CHECK(cm_session_capacity(23,400)==800000);
    CHECK(cm_session_capacity(247,400)==45600000);
    CHECK(cm_session_capacity(UINT16_MAX,UINT32_MAX)==UINT32_MAX);
}
int main(void)
{
    test_pipeline_faults(); test_cancel_and_mailboxes(); test_capacity_auto_and_errors(); test_bounds();
    puts("CM-006 lifecycle/fault tests passed"); return 0;
}
