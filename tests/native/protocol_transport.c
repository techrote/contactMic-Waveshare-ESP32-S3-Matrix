#include "contactmic_ble.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#ifndef VECTOR_DIR
#define VECTOR_DIR "tests/vectors/v1"
#endif
static size_t vector(const char *name, uint8_t *out, size_t capacity)
{
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/%s.hex", VECTOR_DIR, name) > 0);
    FILE *f = fopen(path, "r"); CHECK(f); unsigned int b; size_t n = 0;
    while (fscanf(f, "%2x", &b) == 1) { CHECK(n < capacity); out[n++] = (uint8_t)b; }
    CHECK(feof(f)); CHECK(fclose(f) == 0); return n;
}
static void codec_tests(void)
{
    uint8_t out[526], bytes[526]; size_t n = 0;
    cm_audio a = {.rate_millihz=896800, .descriptor=2, .count=1, .samples={-32768}};
    size_t size = vector("audio_min", bytes, sizeof(bytes));
    CHECK(cm_audio_encode(&a, out, sizeof(out), &n) == CM_OK && n == size);
    CHECK(!memcmp(bytes, out, n));
    a = (cm_audio){.sequence=65535, .first_index=0xfffffffeu, .rate_millihz=7174400,
                   .flags=7, .descriptor=6, .count=5, .samples={-32768,-1,0,1,32767}};
    size = vector("audio_wrap_flags", bytes, sizeof(bytes));
    CHECK(cm_audio_encode(&a,out,sizeof(out),&n)==CM_OK && n==size && !memcmp(out,bytes,n));
    cm_audio decoded = {0}; CHECK(cm_audio_decode(bytes,size,&decoded)==CM_OK);
    CHECK(decoded.sequence==65535 && decoded.first_index==0xfffffffeu && decoded.count==5);
    CHECK(!memcmp(decoded.samples,a.samples,10));
    for (size_t len=0; len<size; ++len) CHECK(cm_audio_decode(bytes,len,&decoded)!=CM_OK);
    const unsigned offsets[] = {0,1,8,12,13,14,15};
    const uint8_t values[] = {2,8,0,0,3,1,1};
    for (size_t i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i) {
        memcpy(out,bytes,size); out[offsets[i]]=values[i];
        if(offsets[i]==8) memset(out+8,0,4);
        cm_audio before=decoded;
        CHECK(cm_audio_decode(out,size,&decoded)!=CM_OK); CHECK(!memcmp(&decoded,&before,sizeof(decoded)));
    }
    a.count=255; a.flags=0;
    for (size_t i=0;i<255;++i) a.samples[i]=(int16_t)i;
    CHECK(cm_audio_encode(&a,out,526,&n)==CM_OK && n==526);
    CHECK(cm_audio_decode(out,n,&decoded)==CM_OK && decoded.count==255);
    memset(out,0xa5,sizeof(out)); CHECK(cm_audio_encode(&a,out,525,&n)==CM_CAPACITY && n==0 && out[0]==0xa5);
    CHECK(cm_audio_capacity(22)==0 && cm_audio_capacity(23)==2 && cm_audio_capacity(24)==2);
    CHECK(cm_audio_capacity(247)==114 && cm_audio_capacity(65535)==255);
    for(unsigned desc=0;desc<256;++desc) CHECK(cm_descriptor_valid((uint8_t)desc)==(desc<7 && desc!=3));
    cm_control c = {0}; size=vector("control_set_axis",bytes,sizeof(bytes));
    CHECK(cm_control_decode(bytes,size,&c)==CM_OK && c.opcode==3 && c.value==2 && c.request_id==0x1234);
    for(size_t len=0;len<size;++len) CHECK(cm_control_decode(bytes,len,&c)!=CM_OK);
    uint8_t saved=bytes[8]; bytes[8]=3; CHECK(cm_control_decode(bytes,size,&c)==CM_INVALID); bytes[8]=saved;
    bytes[6]=1; CHECK(cm_control_decode(bytes,size,&c)==CM_INVALID); bytes[6]=0;
    bytes[1]=10; CHECK(cm_control_decode(bytes,size,&c)==CM_UNSUPPORTED);
    size=vector("control_commit",bytes,sizeof(bytes));
    CHECK(cm_control_decode(bytes,size,&c)==CM_OK && c.session_id==0x12345678u);
    memset(bytes+8,0,4); CHECK(cm_control_decode(bytes,size,&c)==CM_INVALID);
    cm_status status = {.session_id=0x12345678u,.rate_millihz=7174400,.mtu=247,
        .config={2,4,3},.state=CM_PREPARED,.selected_mode=2,.phy=2,
        .source_misses=1,.i2c_errors=2,.ring_overruns=3,.notify_rejections=4,.packets_sent=5,.data_length=251};
    size=vector("status_full",bytes,sizeof(bytes));
    CHECK(cm_status_encode(&status,0,out,sizeof(out),&n)==CM_OK && n==size && !memcmp(out,bytes,n));
    size=vector("status_event",bytes,sizeof(bytes));
    CHECK(cm_status_encode(&status,1,out,sizeof(out),&n)==CM_OK && n==size && !memcmp(out,bytes,n));
    CHECK(cm_status_encode(&status,1,out,19,&n)==CM_CAPACITY && n==0);
    status.state=3; CHECK(cm_status_encode(&status,0,out,sizeof(out),&n)==CM_INVALID);
}
#define QUEUE_CAP 128u
_Static_assert((QUEUE_CAP & (QUEUE_CAP - 1u)) == 0, "power of two");
typedef struct {
    cm_source_sample q[QUEUE_CAP]; size_t head,count; uint32_t index;
    cm_source_counters counters; uint32_t rate; uint8_t mode;
    bool running; cm_result prepare_result,start_result; unsigned stops,prepares;
} source;
static cm_result prepare(void *ctx,const cm_config *config,uint32_t *rate,uint8_t *mode)
{
    source *s=ctx; CHECK(cm_config_valid(config)); ++s->prepares;
    if(s->prepare_result!=CM_OK) return s->prepare_result;
    s->head=s->count=0; s->index=0; *rate=s->rate; *mode=s->mode; return CM_OK;
}
static cm_result start(void *ctx) { source *s=ctx; s->running=s->start_result==CM_OK; return s->start_result; }
static void stop(void *ctx) { source *s=ctx; s->running=false; s->head=s->count=0; ++s->stops; }
static bool peek(void *ctx,size_t offset,cm_source_sample *out)
{ source *s=ctx; if(offset>=s->count) return false; *out=s->q[(s->head+offset)&(QUEUE_CAP-1u)]; return true; }
static void consume(void *ctx,size_t n)
{ source *s=ctx; CHECK(n<=s->count); s->head=(s->head+n)&(QUEUE_CAP-1u); s->count-=n; }
static void counters(void *ctx,cm_source_counters *out) { *out=((source *)ctx)->counters; }
static void produce(source *s,size_t n)
{
    for(size_t i=0;i<n;++i) {
        if(s->count==QUEUE_CAP) ++s->counters.overruns;
        else { s->q[(s->head+s->count)&(QUEUE_CAP-1u)]=(cm_source_sample){s->index,(int16_t)(s->index&0x7fff)}; ++s->count; }
        ++s->index;
    }
}
typedef struct { bool reject,late_fail; size_t calls,bytes; cm_audio frame; cm_transport *t; } sink;
static bool notify(void *ctx,const uint8_t *p,size_t n)
{
    sink *s=ctx; CHECK(n<=244); CHECK(cm_audio_decode(p,n,&s->frame)==CM_OK);
    ++s->calls; s->bytes=n; if(s->late_fail) cm_transport_notify_failed(s->t); return !s->reject;
}
static cm_result command(cm_transport *t,uint8_t op,uint8_t value,uint32_t session)
{ cm_control c={op,0,value,session}; return cm_transport_control(t,&c); }
static void setup(source *s,cm_transport *t)
{
    memset(s,0,sizeof(*s)); s->rate=7174400; s->mode=2;
    cm_source api={s,prepare,start,stop,peek,consume,counters};
    CHECK(cm_transport_init(t,&api,0)==CM_OK); cm_transport_connect(t); cm_transport_subscribe(t,true);
    CHECK(command(t,CM_OP_START,0,0)==CM_OK && t->status.state==CM_PREPARED);
    CHECK(command(t,CM_OP_COMMIT_START,0,t->status.session_id)==CM_OK && t->status.state==CM_STREAMING);
}
static void transport_tests(void)
{
    source s; cm_transport t; setup(&s,&t); sink dst={.t=&t};
    CHECK(cm_transport_pump(&t,notify,&dst)==CM_BUSY && dst.calls==0);
    produce(&s,5); CHECK(cm_transport_pump(&t,notify,&dst)==CM_OK && dst.frame.count==2 && dst.frame.first_index==0);
    cm_transport_mtu(&t,247); CHECK(cm_transport_pump(&t,notify,&dst)==CM_OK && dst.frame.count==3 && dst.frame.sequence==1);
    s.index+=3; s.counters.missed+=3; ++s.counters.i2c_errors; produce(&s,1);
    dst.reject=true; CHECK(cm_transport_pump(&t,notify,&dst)==CM_BUSY && s.count==0);
    CHECK(t.status.notify_rejections==1); dst.reject=false; produce(&s,1);
    CHECK(cm_transport_pump(&t,notify,&dst)==CM_OK && dst.frame.first_index==9 && dst.frame.flags==3 && dst.frame.sequence==3);
    CHECK(t.status.source_misses==3 && t.status.i2c_errors==1);
    produce(&s,2); s.index+=2; produce(&s,2);
    CHECK(cm_transport_pump(&t,notify,&dst)==CM_OK && dst.frame.count==2);
    CHECK(cm_transport_pump(&t,notify,&dst)==CM_OK && dst.frame.count==2 && (dst.frame.flags&1));
    t.sequence=65535; s.index=UINT32_MAX; produce(&s,2);
    CHECK(cm_transport_pump(&t,notify,&dst)==CM_OK && dst.frame.sequence==65535 && dst.frame.first_index==UINT32_MAX && dst.frame.count==2);
    produce(&s,1); CHECK(cm_transport_pump(&t,notify,&dst)==CM_OK && dst.frame.sequence==0 && !(dst.frame.flags&1));
    produce(&s,1000); CHECK(s.count==QUEUE_CAP && s.counters.overruns==872);
    CHECK(cm_transport_pump(&t,notify,&dst)==CM_OK && dst.frame.count==114 && (dst.frame.flags&4));
    CHECK(command(&t,CM_OP_SET_AXIS,1,0)==CM_STATE);
    CHECK(command(&t,CM_OP_STOP,0,0)==CM_OK && !s.running && s.count==0);
    CHECK(command(&t,CM_OP_START,0,0)==CM_STATE); /* Never mix two sessions on one connection. */
    uint32_t old=t.status.session_id; cm_transport_disconnect(&t); cm_transport_connect(&t);
    CHECK(command(&t,CM_OP_SET_AXIS,3,0)==CM_INVALID && t.status.config.axis==2);
    CHECK(command(&t,CM_OP_START,0,0)==CM_OK && t.status.session_id==old+1);
    CHECK(command(&t,CM_OP_COMMIT_START,0,old)==CM_STATE);
    CHECK(command(&t,CM_OP_COMMIT_START,0,t.status.session_id)==CM_STATE); /* subscription required */
    cm_transport_subscribe(&t,true); CHECK(command(&t,CM_OP_COMMIT_START,0,t.status.session_id)==CM_OK);
    CHECK(t.status.source_misses==3 && t.status.i2c_errors==1 && t.status.ring_overruns==872);
    produce(&s,3); cm_transport_subscribe(&t,false); CHECK(t.status.state==CM_STOPPED && s.count==0);
    CHECK(command(&t,CM_OP_RESET_DIAGNOSTICS,0,0)==CM_OK && t.status.source_misses==0);
    cm_transport_snapshot(&t); CHECK(t.status.ring_overruns==0 && t.status.i2c_errors==0);
    s.counters.i2c_errors=0; cm_transport_snapshot(&t); CHECK(t.source_faulted);
    cm_transport_disconnect(&t); cm_transport_connect(&t); CHECK(command(&t,CM_OP_START,0,0)==CM_STATE);
    setup(&s,&t); produce(&s,2); dst=(sink){.t=&t,.late_fail=true};
    CHECK(cm_transport_pump(&t,notify,&dst)==CM_OK && t.status.notify_rejections==1);
    dst.late_fail=false; produce(&s,2); CHECK(cm_transport_pump(&t,notify,&dst)==CM_OK && (dst.frame.flags&1));
    setup(&s,&t); cm_transport_disconnect(&t); cm_transport_connect(&t); t.status.session_id=UINT32_MAX;
    CHECK(command(&t,CM_OP_START,0,0)==CM_STATE && t.exhausted && t.status.state==CM_STOPPED);
    CHECK(cm_transport_init(&t,NULL,0)==CM_OK); cm_transport_connect(&t);
    CHECK(command(&t,CM_OP_START,0,0)==CM_UNSUPPORTED);
    uint8_t req[]={1,3,0x34,0x12,1,0,0,0,2},reply[72]; size_t n=0;
    CHECK(cm_transport_request(&t,req,sizeof(req),reply,sizeof(reply),&n)==CM_OK && n==72);
    CHECK(reply[1]==0x83 && reply[2]==0x34 && reply[4]==CM_OK);
    req[8]=3; cm_config before=t.status.config; memset(reply,0xa5,sizeof(reply));
    CHECK(cm_transport_request(&t,req,sizeof(req),reply,sizeof(reply),&n)==CM_INVALID);
    CHECK(!memcmp(&before,&t.status.config,sizeof(before)) && reply[0]==0xa5);
    req[8]=1; CHECK(cm_transport_request(&t,req,sizeof(req),reply,71,&n)==CM_CAPACITY);
    CHECK(t.status.config.axis==2);
    setup(&s,&t); cm_transport_disconnect(&t); cm_transport_connect(&t);
    s.prepare_result=CM_BUSY;
    CHECK(command(&t,CM_OP_START,0,0)==CM_BUSY && t.preparing);
    CHECK(command(&t,CM_OP_SET_RANGE,4,0)==CM_STATE && t.status.config.range_g==2);
    CHECK(command(&t,CM_OP_STOP,0,0)==CM_OK && !t.preparing);
    s.prepare_result=CM_IO; CHECK(command(&t,CM_OP_START,0,0)==CM_IO && !t.preparing);
    s.prepare_result=CM_OK; CHECK(command(&t,CM_OP_START,0,0)==CM_OK);
    cm_transport_subscribe(&t,true); s.start_result=CM_IO;
    CHECK(command(&t,CM_OP_COMMIT_START,0,t.status.session_id)==CM_IO && t.status.state==CM_STOPPED);
    /* Fixed work, no allocation: fake producer repeatedly outruns/rejects consumer. */
    setup(&s,&t); dst=(sink){.t=&t}; cm_transport_mtu(&t,247);
    for(unsigned i=0;i<100000;++i) {
        produce(&s,17); dst.reject=(i%7==0); (void)cm_transport_pump(&t,notify,&dst);
        CHECK(s.count<=QUEUE_CAP);
    }
    CHECK(t.status.notify_rejections>0 && dst.calls==100000);
}
int main(void) { codec_tests(); transport_tests(); puts("Protocol golden vectors and transport lifecycle/fault/stress tests passed"); return 0; }
