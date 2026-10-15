/* Actual SPSC publication and bounded counter snapshots, across u32 ring wrap.
 * No producer waiting: overflow drops are part of the final accounting check. */
#include "contactmic_acquisition.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#define COUNT 1000000u
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while(0)
static cm_acq a;
static uint32_t done;
static unsigned consumed;
static cm_acq_read_result read_sample(void *unused,int16_t *sample)
{ (void)unused; *sample=(int16_t)((a.next_index-1u)%30000u); return CM_ACQ_READ_OK; }
static void *producer(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<COUNT;i++) {
        cm_acq_result result=cm_acq_service_edges(&a,1);
        CHECK(result==CM_ACQ_OK || result==CM_ACQ_RING_FULL);
    }
    __atomic_store_n(&done,1,__ATOMIC_RELEASE); return NULL;
}
static void *consumer(void *unused)
{
    (void)unused; bool have_last=false; uint32_t last=0; uint64_t previous_reads=0;
    for(;;) {
        cm_acq_sample sample;
        if(cm_acq_peek(&a,0,&sample)) {
            CHECK(sample.value==(int16_t)(sample.index%30000u));
            if(have_last) CHECK((uint32_t)(sample.index-last)>0 && (uint32_t)(sample.index-last)<0x80000000u);
            have_last=true;last=sample.index;++consumed;
            CHECK(cm_acq_consume(&a,1)==1);
        } else if(__atomic_load_n(&done,__ATOMIC_ACQUIRE) && cm_acq_count(&a)==0) break;
        cm_acq_counters snapshot;
        if(cm_acq_try_snapshot(&a,&snapshot)) {
            CHECK(snapshot.successful_reads>=previous_reads);
            previous_reads=snapshot.successful_reads;
        }
    }
    return NULL;
}
int main(void)
{
    CHECK(cm_acq_init(&a,read_sample,NULL)==CM_ACQ_OK);
    CHECK(cm_acq_start_session(&a)==CM_ACQ_OK);
    a.ring.head=a.ring.tail=UINT32_MAX-64u; a.next_index=UINT32_MAX-1000u;
    pthread_t p,c; CHECK(pthread_create(&p,NULL,producer,NULL)==0);CHECK(pthread_create(&c,NULL,consumer,NULL)==0);
    CHECK(pthread_join(p,NULL)==0);CHECK(pthread_join(c,NULL)==0);
    cm_acq_counters snapshot;CHECK(cm_acq_try_snapshot(&a,&snapshot));
    CHECK(snapshot.successful_reads==COUNT && snapshot.drdy_edges==COUNT);
    CHECK(snapshot.ring_overruns+consumed==COUNT && snapshot.inferred_missed_intervals==0);
    puts("CM-006 million-operation SPSC stress passed"); return 0;
}
