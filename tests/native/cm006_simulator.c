/* Deterministic apparatus: synthetic clock/backend, production acquisition,
 * session mailboxes, transport and codecs. No radio/audio devices or sleeps. */
#include "contactmic_session.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
typedef struct {
    cm_acq acquisition; cm_session session; cm_source source; cm_transport transport;
    cm_acq_rate_window window; unsigned mode, read_number;
    bool live, measured, reject; cm_acq_read_result read_result;
    const char *scenario;
} simulation;
static void hex(const uint8_t *bytes, size_t n)
{ for(size_t i=0;i<n;i++) printf("%02x",bytes[i]); }
static void status(simulation *s, const char *event)
{
    cm_transport_snapshot(&s->transport);
    uint8_t bytes[CM_STATUS_SIZE]; size_t n=0;
    CHECK(cm_status_encode(&s->transport.status,0,bytes,sizeof(bytes),&n)==CM_OK);
    printf("{\"scenario\":\"%s\",\"event\":\"%s\",\"status\":\"",s->scenario,event);
    hex(bytes,n); puts("\"}");
}
static cm_acq_read_result read_sample(void *ctx, int16_t *value)
{
    simulation *s=ctx; cm_acq_read_result rc=s->read_result; s->read_result=CM_ACQ_READ_OK;
    const int16_t extremes[]={-32768,32767,-1,0,1};
    *value=extremes[s->read_number++ % 5]; return rc;
}
static cm_result begin(void *ctx,const cm_config *c,uint8_t mode)
{
    simulation *s=ctx; CHECK(cm_config_valid(c)); s->mode=mode; s->live=false; s->measured=false;
    CHECK(cm_acq_start_session(&s->acquisition)==CM_ACQ_OK);
    CHECK(cm_acq_rate_begin(&s->acquisition,1000000,&s->window)); return CM_OK;
}
static cm_result ready(void *ctx,uint32_t *rate)
{
    simulation *s=ctx;
    if(!s->measured) {
        const unsigned edges[]={225,900,1800}; /* fake 250 ms calibration, not hardware ODR proof */
        for(unsigned i=0;i<edges[s->mode];i++) {
            CHECK(cm_acq_service_edges(&s->acquisition,1)==CM_ACQ_OK);
            CHECK(cm_acq_consume(&s->acquisition,1)==1);
        }
        CHECK(cm_acq_rate_finish(&s->acquisition,&s->window,1250000)); s->measured=true;
    }
    *rate=cm_acq_measured_rate_millihz(&s->acquisition); return CM_OK;
}
static cm_result publish(void *ctx)
{
    simulation *s=ctx; CHECK(cm_acq_rebase_publication(&s->acquisition)==CM_ACQ_OK);
    s->live=true; return CM_OK;
}
static bool published(void *ctx) { return ((simulation *)ctx)->live; }
static cm_result stop(void *ctx)
{
    simulation *s=ctx; s->live=false;
    CHECK(cm_acq_stop_session(&s->acquisition)==CM_ACQ_OK); return CM_OK;
}
static cm_result control(simulation *s,uint8_t op,uint8_t value)
{
    const cm_control command={.opcode=op,.value=value,.session_id=s->transport.status.session_id};
    return cm_transport_control(&s->transport,&command);
}
static void connect(simulation *s)
{
    cm_transport_connect(&s->transport); cm_transport_mtu(&s->transport,247);
    CHECK(!cm_session_monitor(&s->session,&s->transport,0,400));
}
static void initialize(simulation *s,const char *scenario)
{
    memset(s,0,sizeof(*s)); s->scenario=scenario;
    CHECK(cm_acq_init(&s->acquisition,read_sample,s)==CM_ACQ_OK);
    const cm_session_backend b={s,begin,ready,publish,published,stop};
    CHECK(cm_session_init(&s->session,&s->acquisition,&b,CM_POLICY_HIGH)==CM_OK);
    s->source=cm_session_source(&s->session);
    CHECK(cm_transport_init(&s->transport,&s->source,100)==CM_OK); connect(s);
}
static void start(simulation *s,uint8_t policy)
{
    CHECK(control(s,CM_OP_SET_POLICY,policy)==CM_OK);
    CHECK(control(s,CM_OP_START,0)==CM_BUSY);
    CHECK(control(s,CM_OP_SET_AXIS,CM_AXIS_Y)==CM_STATE); /* cannot mutate pending preparation */
    cm_session_worker_step(&s->session,0); cm_session_worker_step(&s->session,250000);
    CHECK(control(s,CM_OP_START,0)==CM_OK);
    cm_transport_subscribe(&s->transport,true);
    CHECK(control(s,CM_OP_COMMIT_START,0)==CM_OK);
    cm_session_worker_step(&s->session,260000);
    CHECK(!cm_session_monitor(&s->session,&s->transport,300000,400));
    status(s,"session");
}
static bool notify(void *ctx,const uint8_t *bytes,size_t n)
{
    simulation *s=ctx;
    printf("{\"scenario\":\"%s\",\"event\":\"%s\",\"audio\":\"",s->scenario,s->reject?"rejected":"audio");
    hex(bytes,n); puts("\"}"); return !s->reject;
}
static void produce(simulation *s,unsigned n)
{
    for(unsigned i=0;i<n;i++) {
        cm_acq_result rc=cm_acq_service_edges(&s->acquisition,1);
        CHECK(rc==CM_ACQ_OK || rc==CM_ACQ_RING_FULL);
    }
    CHECK(cm_acq_count(&s->acquisition)<=CM_ACQ_RING_CAPACITY);
}
static void pump_all(simulation *s)
{
    for(unsigned i=0;cm_acq_count(&s->acquisition);i++) {
        CHECK(i<256); CHECK(cm_transport_pump(&s->transport,notify,s)==CM_OK);
    }
}
static void faults(void)
{
    simulation s; initialize(&s,"faults"); start(&s,CM_POLICY_HIGH);
    CHECK(control(&s,CM_OP_SET_RANGE,4)==CM_STATE);
    produce(&s,8); pump_all(&s);
    CHECK(cm_acq_service_edges(&s.acquisition,4)==CM_ACQ_OK); pump_all(&s);
    s.read_result=CM_ACQ_READ_IO; CHECK(cm_acq_service_edges(&s.acquisition,1)==CM_ACQ_IO);
    produce(&s,4); pump_all(&s);
    produce(&s,300); s.reject=true;
    CHECK(cm_transport_pump(&s.transport,notify,&s)==CM_BUSY); s.reject=false;
    pump_all(&s); produce(&s,4); pump_all(&s); status(&s,"end");
    produce(&s,3); cm_transport_disconnect(&s.transport);
    cm_source_sample sample; CHECK(!s.source.peek(s.source.context,0,&sample));
    s.scenario="reconnect"; connect(&s);
    CHECK(control(&s,CM_OP_SET_AXIS,CM_AXIS_X)==CM_OK);
    CHECK(control(&s,CM_OP_SET_RANGE,4)==CM_OK);
    start(&s,CM_POLICY_MEDIUM); produce(&s,8); pump_all(&s); status(&s,"end");
}
static void wrap(void)
{
    simulation s; initialize(&s,"wrap"); start(&s,CM_POLICY_HIGH);
    /* Explicit boundary injection avoids a four-billion-iteration test. These
     * are synthetic internal seeds, not normal start state or hardware history. */
    s.acquisition.next_index=UINT32_MAX-2u; s.transport.sequence=UINT16_MAX-1u;
    produce(&s,3); pump_all(&s); produce(&s,2); pump_all(&s); produce(&s,2); pump_all(&s); status(&s,"end");
}
static void clean(void)
{
    simulation s; initialize(&s,"clean"); start(&s,CM_POLICY_HIGH);
    const unsigned counts[]={1,7,31,2,114,5};
    for(size_t i=0;i<sizeof(counts)/sizeof(counts[0]);i++) { produce(&s,counts[i]); pump_all(&s); }
    status(&s,"end");
}
static void adaptive(void)
{
    simulation s; initialize(&s,"auto_high"); start(&s,CM_POLICY_AUTO);
    s.read_result=CM_ACQ_READ_IO; CHECK(cm_acq_service_edges(&s.acquisition,1)==CM_ACQ_IO);
    CHECK(cm_session_monitor(&s.session,&s.transport,1300000,400)); status(&s,"boundary");
    CHECK(control(&s,CM_OP_START,0)==CM_STATE); /* no metadata splice on the used connection */
    cm_transport_disconnect(&s.transport); connect(&s); s.scenario="auto_medium"; start(&s,CM_POLICY_AUTO);
    cm_transport_notify_failed(&s.transport);
    CHECK(cm_session_monitor(&s.session,&s.transport,1300000,400)); status(&s,"boundary");
    cm_transport_disconnect(&s.transport); connect(&s); s.scenario="auto_safe"; start(&s,CM_POLICY_AUTO);
    produce(&s,3); pump_all(&s);
    cm_transport_mtu(&s.transport,23);
    CHECK(cm_session_monitor(&s.session,&s.transport,300001,400)); status(&s,"boundary");
    cm_transport_disconnect(&s.transport); cm_transport_connect(&s.transport);
    CHECK(!cm_session_monitor(&s.session,&s.transport,0,400));
    CHECK(control(&s,CM_OP_START,0)==CM_CAPACITY); status(&s,"capacity_refused");
}
int main(void)
{
    faults(); wrap(); clean(); adaptive();
    return ferror(stdout)?1:0;
}
