#include "cm_usb.h"
#include "cm_usb_descriptors.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
typedef struct {
    cmu_core core;
    cmu_sample samples[256];
    uint32_t head, tail, next, measured;
    cmu_source_counts counts;
    unsigned begins, stops, publications;
    bool ready, live, hold_ack, cancel_begin, fail, snapshot_busy;
} fixture;
static size_t count(void *p) { fixture *f=p; return f->head-f->tail; }
static bool peek(void *p,size_t n,cmu_sample *out)
{ fixture *f=p; if(n>=count(p))return false; *out=f->samples[(f->tail+n)%256]; return true; }
static size_t consume(void *p,size_t n)
{ fixture *f=p; if(n>count(p))n=count(p); f->tail+=n; return n; }
static bool snapshot(void *p,cmu_source_counts *out)
{ fixture *f=p; if(f->snapshot_busy)return false; *out=f->counts; return true; }
static cmu_result begin(void *p,const cmu_config *cfg,uint32_t mode)
{
    fixture *f=p; CHECK(cmu_config_valid(cfg) && mode<=CMU_HIGH); ++f->begins;
    if(f->cancel_begin) { f->cancel_begin=false; CHECK(cmu_request_stream(&f->core,false,CMU_REASON_STOP,100)==CMU_OK); }
    return f->fail?CMU_IO:CMU_OK;
}
static cmu_result ready(void *p,uint32_t *rate)
{ fixture *f=p; *rate=f->measured; return f->ready?CMU_OK:CMU_BUSY; }
static cmu_result publish(void *p)
{ fixture *f=p; ++f->publications; f->head=f->tail=0; f->next=0; f->live=true; return CMU_OK; }
static bool published(void *p) { fixture *f=p; return f->live && !f->hold_ack; }
static cmu_result stop(void *p)
{ fixture *f=p; ++f->stops; f->head=f->tail=0; f->live=false; return f->fail?CMU_IO:CMU_OK; }
static void init(fixture *f,uint32_t rate)
{
    memset(f,0,sizeof(*f)); f->ready=true; f->measured=rate;
    cmu_backend b={f,begin,ready,publish,published,stop,peek,consume,count,snapshot};
    CHECK(cmu_init(&f->core,&b)==CMU_OK);
}
static void sample(fixture *f,int16_t x)
{
    ++f->counts.intervals; ++f->counts.reads;
    if(count(f)==256) ++f->counts.overruns;
    else f->samples[(f->head++)%256]=(cmu_sample){f->next,x};
    ++f->next;
}
static void prepare(fixture *f)
{
    CHECK(cmu_request_stream(&f->core,true,CMU_REASON_NONE,0)==CMU_OK);
    CHECK(f->begins==0);
    cmu_worker_step(&f->core,0);
    cmu_worker_step(&f->core,250000);
    cmu_tick(&f->core,250000);
    CHECK(f->core.state==CMU_PRIMING);
}
static void prime(fixture *f,int16_t x)
{
    for(unsigned i=0;i<f->core.target+4;i++)sample(f,x);
    cmu_tick(&f->core,f->core.state_started_us+1000); CHECK(f->core.state==CMU_RUNNING);
}
static int16_t word(const uint8_t *p) { return (int16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8)); }
static void descriptors(void)
{
    CHECK(cmu_device_descriptor[4]==0xEF && cmu_device_descriptor[17]==1);
    const uint8_t *d=cmu_configuration_descriptor;
    CHECK(((unsigned)d[2]|((unsigned)d[3]<<8))==CMU_CONFIG_DESC_LEN && d[4]==4);
    size_t at=0; unsigned interfaces=0, ep=0, iad=0, ac_total=0;
    unsigned seen[256]={0}; int itf=-1, alt=-1;
    while(at<CMU_CONFIG_DESC_LEN) {
        unsigned n=d[at]; CHECK(n>=2 && at+n<=CMU_CONFIG_DESC_LEN);
        if(d[at+1]==11) ++iad;
        if(d[at+1]==4) { ++interfaces; itf=d[at+2]; alt=d[at+3]; }
        if(itf==0 && d[at+1]==0x24)ac_total+=n;
        if(d[at+1]==5) {
            ++ep; unsigned address=d[at+2]; CHECK(!seen[address]++);
            if(address==0x81)CHECK(itf==1 && alt==1 && d[at+3]==0x0D && d[at+4]==96 && d[at+6]==1);
        }
        at+=n;
    }
    CHECK(at==CMU_CONFIG_DESC_LEN && iad==2 && interfaces==5 && ep==4 && ac_total==46);
    uint8_t reply[14]; uint16_t n=0;
    CHECK(cmu_uac_get(0xA1,1,0x100,0x400,4,reply,&n) && n==4);
    CHECK(reply[0]==0x80 && reply[1]==0xBB);
    CHECK(cmu_uac_get(0xA1,2,0x100,0x400,14,reply,&n) && n==14 && reply[0]==1);
    CHECK(!memcmp(reply+2,reply+6,4) && !reply[10] && !reply[11] && !reply[12] && !reply[13]);
    CHECK(cmu_uac_get(0xA1,2,0x100,0x400,2,reply,&n) && n==2);
    CHECK(cmu_uac_get(0xA1,1,0x200,0x400,1,reply,&n) && reply[0]==1);
    CHECK(!cmu_uac_get(0x21,1,0x100,0x400,4,reply,&n));
    CHECK(!cmu_uac_get(0xA1,1,0x101,0x400,4,reply,&n));
    CHECK(!cmu_uac_get(0xA1,1,0x100,0x401,4,reply,&n));
    CHECK(!cmu_uac_get(0xA1,1,0x100,0x400,0,reply,&n));
    CHECK(cmu_uac_alt_valid(1,11,1,1,0) && cmu_uac_alt_valid(1,11,0,1,0));
    CHECK(!cmu_uac_alt_valid(1,11,256,1,0) && !cmu_uac_alt_valid(1,11,1,257,0));
}
static void controls_and_dsp(void)
{
    fixture f; init(&f,1000000); uint32_t id;
    const char *valid="CONFIG 4 0 4 1 2000 0";
    CHECK(cmu_command_line(&f.core,valid,strlen(valid),&id)==CMU_OK && id==4);
    CHECK(f.core.config.axis==0 && f.core.config.gain_milli==2000);
    const char *bad[]={"", "STATUS 0", "STATUS 1 rubbish", "CONFIG 1 0 4 1 1000 1 2", "STATUS -1", "STATUS 4294967296", "CONFIG 1 0 3 1 1000 0"};
    cmu_config before=f.core.config;
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        CHECK(cmu_command_line(&f.core,bad[i],strlen(bad[i]),&id)==CMU_INVALID);
        CHECK(!memcmp(&before,&f.core.config,sizeof(before)));
    }
    prepare(&f); prime(&f,INT16_MIN);
    uint8_t bytes[CMU_FRAME_BYTES]; cmu_render(&f.core,bytes,252000);
    for(unsigned i=0;i<48;i++)CHECK(word(bytes+2*i)==INT16_MIN);
    CHECK(f.core.counters.dsp_clips>0 && f.core.counters.raw_clips>0);
    CHECK(cmu_command_line(&f.core,valid,strlen(valid),&id)==CMU_BUSY);
    CHECK(cmu_command_line(&f.core,"STATUS 17",9,&id)==CMU_OK && id==17);
    cmu_usb_written(&f.core,95); CHECK(f.core.counters.usb_short_writes==1);
    f.snapshot_busy=true; cmu_source_counts saved=f.core.source;
    cmu_snapshot_source(&f.core); CHECK(!memcmp(&saved,&f.core.source,sizeof(saved)));
}
static void lifecycle(void)
{
    fixture f; init(&f,900000); f.cancel_begin=true;
    CHECK(cmu_request_stream(&f.core,true,CMU_REASON_NONE,0)==CMU_OK);
    cmu_worker_step(&f.core,0); cmu_worker_step(&f.core,250000);
    CHECK(!f.core.requested);
    uint8_t b[96]; cmu_render(&f.core,b,250001); for(unsigned i=0;i<96;i++)CHECK(!b[i]);
    /* Ten starts without a physical unplug; every start gets a new generation. */
    uint32_t last=f.core.generation;
    for(unsigned i=0;i<10;i++) {
        CHECK(cmu_request_stream(&f.core,true,CMU_REASON_NONE,0)==CMU_OK);
        cmu_worker_step(&f.core,0); cmu_worker_step(&f.core,250000); cmu_tick(&f.core,250000);
        CHECK(f.core.generation>last); last=f.core.generation;
        CHECK(f.core.state==CMU_PRIMING); prime(&f,100);
        CHECK(cmu_request_stream(&f.core,false,CMU_REASON_STOP,300000)==CMU_OK);
        cmu_render(&f.core,b,300001); for(unsigned k=0;k<96;k++)CHECK(!b[k]);
    }
    f.core.generation=UINT32_MAX;
    CHECK(cmu_request_stream(&f.core,true,CMU_REASON_NONE,0)==CMU_EXHAUSTED);
    CHECK(cmu_request_stream(&f.core,false,CMU_REASON_RESET,0)==CMU_OK);
    init(&f,900000); f.hold_ack=true;
    CHECK(cmu_request_stream(&f.core,true,CMU_REASON_NONE,0)==CMU_OK);
    cmu_worker_step(&f.core,0); cmu_worker_step(&f.core,250000);
    f.hold_ack=false; cmu_worker_step(&f.core,2250000); cmu_tick(&f.core,2250000);
    CHECK(f.core.state==CMU_FAILED); /* exact deadline, no late resurrection */
    unsigned stops=f.stops;
    cmu_worker_step(&f.core,2250001); cmu_worker_step(&f.core,2250002);
    CHECK(f.stops<=stops+1); /* one explicit stop, not a retry loop */
    init(&f,0); CHECK(cmu_request_stream(&f.core,true,CMU_REASON_NONE,0)==CMU_OK);
    cmu_worker_step(&f.core,0); cmu_worker_step(&f.core,1); cmu_tick(&f.core,1);
    CHECK(f.core.state==CMU_FAILED);
    init(&f,900000); f.ready=false;
    CHECK(cmu_request_stream(&f.core,true,CMU_REASON_NONE,0)==CMU_OK);
    cmu_worker_step(&f.core,0); cmu_worker_step(&f.core,CMU_PREPARE_US); cmu_tick(&f.core,CMU_PREPARE_US);
    CHECK(f.core.state==CMU_FAILED);
}
static void faults(void)
{
    fixture f; init(&f,1000000); prepare(&f); prime(&f,1000);
    uint8_t b[96];
    /* Drop three source intervals, then consume through the discontinuity. */
    f.next+=3; f.counts.intervals+=3; f.counts.missed+=3;
    for(unsigned i=0;i<80;i++) { sample(&f,1000); cmu_render(&f.core,b,252000+i*1000); }
    CHECK(f.core.counters.source_gaps==3 && f.core.counters.gap_zeros==3);
    CHECK(f.core.source.missed==3);
    /* Large gap must be bounded, not a gap-sized loop/allocation. */
    f.next+=1000000; f.counts.intervals+=1000000; f.counts.missed+=1000000;
    for(unsigned i=0;i<100;i++) { sample(&f,100); cmu_render(&f.core,b,400000+i*1000); }
    CHECK(f.core.counters.large_gap_resets>0);
    /* Backpressure is explicit drop + bounded resynchronization. */
    for(unsigned i=0;i<300;i++)sample(&f,100);
    cmu_render(&f.core,b,600000);
    CHECK(f.core.counters.queue_discards>0 && f.counts.overruns>0 && count(&f)<=224);
    /* Source-index wrap on a running generation. */
    CHECK(cmu_request_stream(&f.core,false,CMU_REASON_STOP,700000)==CMU_OK);
    cmu_worker_step(&f.core,700000);
    CHECK(cmu_request_stream(&f.core,true,CMU_REASON_NONE,800000)==CMU_OK);
    cmu_worker_step(&f.core,800000); cmu_worker_step(&f.core,1050000); cmu_tick(&f.core,1050000);
    f.core.have_index=true; f.core.expected_index=UINT32_MAX-5; f.next=UINT32_MAX-5;
    prime(&f,100); CHECK(f.next<100);
    cmu_render(&f.core,b,1052000);
    CHECK(f.core.counters.duplicate_source==0);
}
static void jitter_and_dc(void)
{
    fixture f; init(&f,7174400); prepare(&f); uint8_t b[96]; double credit=0;
    const int jitter[]={0,130,-90,80,-160,150,-40};
    for(unsigned ms=1;ms<12000;ms++) {
        credit+=7.1744;
        while(credit>=1){sample(&f,16000);credit-=1;}
        /* USB transfers still one/frame; MCU callback arrival has bounded jitter. */
        cmu_render(&f.core,b,250000ull+ms*1000ull+jitter[ms%7]);
    }
    CHECK(f.core.state==CMU_RUNNING&&f.core.counters.starvation_silence==0);
    for(unsigned i=0;i<48;i++)CHECK(abs(word(b+2*i))<=1); /* steady gravity rejected */
    uint64_t misses=f.core.counters.usb_late_frames;
    for(unsigned i=0;i<150;i++)sample(&f,100);
    cmu_render(&f.core,b,12500000); /* delayed USB service must remain visible */
    CHECK(f.core.counters.usb_late_frames>misses);
    CHECK(f.core.counters.queue_discards>0||count(&f)<=224);
    f.core.counters.usb_short_writes=UINT64_MAX;
    cmu_usb_written(&f.core,0);CHECK(f.core.counters.usb_short_writes==UINT64_MAX);
    /* AUTO can request a lower generation without changing USB's 48k format. */
    init(&f,1000000);f.core.config.policy=CMU_AUTO;f.core.auto_ceiling=CMU_HIGH;
    prepare(&f);prime(&f,100);uint32_t previous=f.core.generation;
    ++f.counts.i2c;++f.counts.missed;++f.counts.intervals;
    cmu_tick(&f.core,1300000);
    CHECK(f.core.generation>previous&&f.core.mode==CMU_MEDIUM&&f.core.rate==0);
    CHECK(f.core.source.i2c==1&&f.core.reason==CMU_REASON_HEALTH);
}
static void clocks(uint32_t rate,int ppm,unsigned milliseconds)
{
    fixture f; init(&f,rate); prepare(&f);
    double credit=0;
    uint8_t b[96]; unsigned min=256,max=0;
    for(unsigned ms=1;ms<=milliseconds;ms++) {
        credit+=(double)rate/1000000.0*(1.0+ppm/1000000.0);
        while(credit>=1) { sample(&f,(int16_t)(f.next%20000)); credit-=1; }
        cmu_render(&f.core,b,250000ull+ms*1000ull);
        if(ms>2000) {
            unsigned q=(unsigned)count(&f); if(q<min)min=q; if(q>max)max=q;
            CHECK(f.core.state==CMU_RUNNING);
        }
    }
    CHECK(f.core.counters.starvation_silence==0 && f.core.counters.queue_discards==0);
    CHECK(f.counts.overruns==0 && min>0 && max<224);
    CHECK(abs(f.core.correction_ppm-ppm)<750);
    printf("clock rate=%u drift=%d ppm final=%d ppm queue=%u..%u frames=%u\n",rate,ppm,f.core.correction_ppm,min,max,milliseconds);
}
int main(int argc,char **argv)
{
    (void)argv;
    descriptors(); controls_and_dsp(); lifecycle(); faults(); jitter_and_dc();
    unsigned duration=argc>1?600000u:30000u;
    clocks(896800,-5000,duration); clocks(896800,5000,duration);
    clocks(3587200,-5000,duration); clocks(3587200,5000,duration);
    clocks(7174400,-5000,duration); clocks(7174400,5000,duration);
    puts("CM-009 core/descriptor/lifecycle/clock qualification passed"); return 0;
}
