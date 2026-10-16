/* The real accepted acquisition core feeds the real USB DSP/rate converter.
 * Only sensor values, lifecycle scheduling and USB time are synthetic. */
#include "contactmic_acquisition.h"
#include "cm_usb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef struct {cm_acq acquisition;cmu_core usb;cm_acq_read_result next;bool published;} fixture;
static cm_acq_read_result read_sample(void *p,int16_t *out)
{fixture *f=p;*out=1234;cm_acq_read_result r=f->next;f->next=CM_ACQ_READ_OK;return r;}
static cmu_result begin(void *p,const cmu_config *c,uint32_t mode)
{fixture *f=p;CHECK(cmu_config_valid(c)&&mode<=CMU_HIGH);return cm_acq_start_session(&f->acquisition)==CM_ACQ_OK?CMU_OK:CMU_IO;}
static cmu_result ready(void *p,uint32_t *rate){(void)p;*rate=1000000;return CMU_OK;}
static cmu_result publish(void *p)
{fixture *f=p;CHECK(cm_acq_rebase_publication(&f->acquisition)==CM_ACQ_OK);f->published=true;return CMU_OK;}
static bool published(void *p){return ((fixture*)p)->published;}
static cmu_result stop(void *p)
{fixture *f=p;f->published=false;return cm_acq_stop_session(&f->acquisition)==CM_ACQ_OK?CMU_OK:CMU_IO;}
static bool peek(void *p,size_t n,cmu_sample *out)
{cm_acq_sample s;if(!cm_acq_peek(&((fixture*)p)->acquisition,n,&s))return false;*out=(cmu_sample){s.index,s.value};return true;}
static size_t count(void *p){return cm_acq_count(&((fixture*)p)->acquisition);}
static size_t consume(void *p,size_t n){return cm_acq_consume(&((fixture*)p)->acquisition,n);}
static bool snapshot(void *p,cmu_source_counts *out)
{cm_acq_counters c;if(!cm_acq_try_snapshot(&((fixture*)p)->acquisition,&c))return false;
*out=(cmu_source_counts){c.drdy_edges,c.successful_reads,c.inferred_missed_intervals,c.i2c_failures,c.ring_overruns,c.source_faults};return true;}
int main(void)
{
    fixture f={0};CHECK(cm_acq_init(&f.acquisition,read_sample,&f)==CM_ACQ_OK);
    cmu_backend backend={&f,begin,ready,publish,published,stop,peek,consume,count,snapshot};
    CHECK(cmu_init(&f.usb,&backend)==CMU_OK);
    CHECK(cmu_request_stream(&f.usb,true,CMU_REASON_NONE,0)==CMU_OK);
    cmu_worker_step(&f.usb,0);
    /* Calibration prefix must not leak into the first USB source index. */
    CHECK(cm_acq_service_edges(&f.acquisition,20)==CM_ACQ_OK);
    cmu_worker_step(&f.usb,250000);cmu_tick(&f.usb,250000);
    for(unsigned i=0;i<24;i++)CHECK(cm_acq_service_edges(&f.acquisition,1)==CM_ACQ_OK);
    uint8_t pcm[96];cmu_render(&f.usb,pcm,251000);CHECK(f.usb.state==CMU_RUNNING);
    uint64_t baseline=f.usb.source.missed;
    CHECK(cm_acq_service_edges(&f.acquisition,4)==CM_ACQ_OK);
    f.next=CM_ACQ_READ_IO;CHECK(cm_acq_service_edges(&f.acquisition,1)==CM_ACQ_IO);
    for(unsigned i=0;i<100;i++) {
        CHECK(cm_acq_service_edges(&f.acquisition,1)==CM_ACQ_OK);
        cmu_render(&f.usb,pcm,252000u+i*1000u);
    }
    CHECK(f.usb.source.missed-baseline==4 && f.usb.source.i2c==1);
    CHECK(f.usb.counters.source_gaps==4 && f.usb.counters.gap_zeros==4);
    /* Actual production ring cannot wait for USB and records every overflow. */
    for(unsigned i=0;i<300;i++) (void)cm_acq_service_edges(&f.acquisition,1);
    cmu_render(&f.usb,pcm,500000);CHECK(f.usb.source.overruns>0&&f.usb.counters.queue_discards>0);
    cmu_source_counts before=f.usb.source;
    CHECK(cmu_request_stream(&f.usb,false,CMU_REASON_DETACH,600000)==CMU_OK);
    cmu_worker_step(&f.usb,600000);CHECK(count(&f)==0);
    CHECK(cmu_request_stream(&f.usb,true,CMU_REASON_NONE,700000)==CMU_OK);
    cmu_worker_step(&f.usb,700000);cmu_worker_step(&f.usb,950000);cmu_tick(&f.usb,950000);
    for(unsigned i=0;i<24;i++)CHECK(cm_acq_service_edges(&f.acquisition,1)==CM_ACQ_OK);
    cmu_render(&f.usb,pcm,951000);
    CHECK(f.usb.state==CMU_RUNNING&&f.usb.source.i2c==before.i2c&&f.usb.source.overruns==before.overruns);
    CHECK(f.usb.expected_index<24);
    puts("CM-009 production acquisition -> USB PCM loss/lifecycle qualification passed");return 0;
}
