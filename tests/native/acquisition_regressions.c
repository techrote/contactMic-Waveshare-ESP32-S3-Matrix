#include "contactmic_acquisition.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static cm_acq_read_result read_one(void *unused,int16_t *value)
{ (void)unused; *value=1; return CM_ACQ_READ_OK; }
int main(void)
{
    cm_acq a; cm_acq_rate_window window;
    CHECK(cm_acq_init(&a,read_one,NULL)==CM_ACQ_OK);
    CHECK(cm_acq_start_session(&a)==CM_ACQ_OK);
    CHECK(cm_acq_rate_begin(&a,1,&window));
    CHECK(cm_acq_service_edges(&a,UINT64_MAX / 1000000000ull)==CM_ACQ_OK);
    CHECK(cm_acq_rate_finish(&a,&window,1000000000001ull));
    /* Pre-CM-006 adds elapsed/2 to a near-UINT64_MAX product and reports zero. */
    CHECK(cm_acq_measured_rate_millihz(&a)==18446744u);
#ifndef CM006_BASELINE
    uint32_t rate=777;
    CHECK(!cm_acq_calculate_rate(UINT64_MAX,1000,&rate) && rate==777);
    CHECK(!cm_acq_calculate_rate(10,0,&rate) && rate==777);
    CHECK(!cm_acq_calculate_rate(10,1,&rate) && rate==777);
    CHECK(cm_acq_calculate_rate(1,2000000000ull,&rate) && rate==1);
    CHECK(cm_acq_calculate_rate(0,UINT64_MAX,&rate) && rate==0);
    cm_acq_counters out={.drdy_edges=123};
    __atomic_store_n(&a.counter_seq,1,__ATOMIC_SEQ_CST);
    CHECK(!cm_acq_try_snapshot(&a,&out) && out.drdy_edges==123);
    __atomic_store_n(&a.counter_seq,2,__ATOMIC_SEQ_CST);
    CHECK(cm_acq_try_snapshot(&a,&out));
    CHECK(cm_acq_rebase_publication(&a)==CM_ACQ_OK);
    CHECK(cm_acq_count(&a)==0 && cm_acq_measured_rate_millihz(&a)==18446744u);
#endif
    puts("CM-006 acquisition regression tests passed"); return 0;
}
