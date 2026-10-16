/* Compile the production adapter against explicit scheduler/GPIO/QMI fakes.
 * This tests ordering, not timing feasibility or a physical sensor waveform. */
#include <stdio.h>
#include <stdlib.h>
#include "../../firmware/components/contactmic_acquisition/contactmic_acquisition_idf.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
unsigned cm_test_critical;
static int64_t clock_us;
static bool irq_enabled, level_high=true, interrupt_in_read, io_failure;
static unsigned reads;
static unsigned probes;
static bool probing;
esp_err_t gpio_get_io_config(gpio_num_t pin,gpio_io_config_t *io)
{CHECK(pin==13);*io=(gpio_io_config_t){.fun_sel=1,.ie=1};probing=true;return ESP_OK;}
cmq_result cmq_diagnostic_snapshot(cmq_device *d,uint8_t out[12])
{CHECK(!cm_test_critical&&d->state==CMQ_RUNNING);memset(out,++probes,12);return CMQ_OK;}
static void (*handler)(void *);
static void *handler_context;
static int task_storage, gate_storage;
static void edge(void) { if (irq_enabled && handler) handler(handler_context); }
int64_t esp_timer_get_time(void) { if(probing)clock_us+=10;return clock_us; }
BaseType_t xTaskCreate(void (*fn)(void *),const char *name,uint32_t stack,void *ctx,unsigned priority,TaskHandle_t *out)
{ (void)fn;(void)name;(void)ctx; CHECK(stack==4096 && priority==configMAX_PRIORITIES-2); *out=&task_storage; return pdPASS; }
void vTaskDelete(TaskHandle_t t) { CHECK(t==&task_storage); }
void vTaskNotifyGiveFromISR(TaskHandle_t t,BaseType_t *w) { CHECK(t==&task_storage); *w=pdFALSE; }
uint32_t ulTaskNotifyValueClear(TaskHandle_t t,uint32_t v) { CHECK(t==&task_storage);(void)v;return 0; }
uint32_t ulTaskNotifyTake(BaseType_t b,TickType_t t) { (void)b;(void)t;return 0; }
void xTaskNotifyGive(TaskHandle_t t) { CHECK(t==&task_storage); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &gate_storage; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t t) { CHECK(s==&gate_storage);(void)t;return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { CHECK(s==&gate_storage);return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { CHECK(s==&gate_storage); }
esp_err_t gpio_config(const gpio_config_t *g) { CHECK(g->pin_bit_mask==(1ull<<13) && !g->pull_up_en);return ESP_OK; }
esp_err_t gpio_install_isr_service(int flags) { CHECK(flags==0);return ESP_OK; }
void gpio_uninstall_isr_service(void) { }
esp_err_t gpio_isr_handler_add(gpio_num_t pin,void (*fn)(void *),void *ctx)
{ CHECK(pin==13);handler=fn;handler_context=ctx;return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t pin) { CHECK(pin==13);handler=NULL;return ESP_OK; }
esp_err_t gpio_intr_enable(gpio_num_t pin) { CHECK(pin==13);irq_enabled=true;return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t pin) { CHECK(pin==13);irq_enabled=false;return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { CHECK(pin==13);return level_high; }
cmq_result cmq_configure(cmq_device *d,const cmq_config *c)
{ CHECK(!cm_test_critical && c->drdy_enabled);d->config=*c;d->state=CMQ_CONFIGURED;return CMQ_OK; }
cmq_result cmq_start(cmq_device *d)
{ CHECK(!cm_test_critical && d->state==CMQ_CONFIGURED);clock_us+=150000;d->state=CMQ_RUNNING;return CMQ_OK; }
cmq_result cmq_stop(cmq_device *d) { CHECK(!cm_test_critical); d->state=CMQ_CONFIGURED;return CMQ_OK; }
cmq_result cmq_read_selected(cmq_device *d,int16_t *v)
{
    CHECK(!cm_test_critical && d->state==CMQ_RUNNING); ++reads;
    if (io_failure) { io_failure=false;return CMQ_IO; }
    if (interrupt_in_read) { interrupt_in_read=false; edge(); }
    *v=123; return CMQ_OK;
}
int main(void)
{
    cmq_device device={.state=CMQ_IDLE}; cm_acq_idf *a=NULL;
    CHECK(cm_acq_idf_create(&device,&a)==ESP_OK);
    CHECK(cm_acq_idf_configure(a,CM_ACQ_POLICY_SAFE,CMQ_Z,2,CM_ACQ_MODE_SAFE)==ESP_OK);
    CHECK(cm_acq_idf_prepare(a)==ESP_OK && irq_enabled);
    CHECK(!cm_acq_idf_publishing(a));
    for(unsigned i=0;i<300;i++) {
        clock_us+=1000; edge(); acquisition_cycle(a);
        CHECK(cm_acq_count(&a->core)==0); /* no calibration overflow/backlog */
    }
    CHECK(cm_acq_idf_measurement_ready(a));
    CHECK(cm_acq_idf_measured_rate_millihz(a)==1000000);
    cm_acq_counters c; cm_acq_snapshot(&a->core,&c);
    CHECK(c.ring_overruns==0 && c.successful_reads==300);
    CHECK(cm_acq_idf_publish(a)==ESP_OK && !cm_acq_idf_publishing(a));
    acquisition_cycle(a); CHECK(cm_acq_idf_publishing(a));
    clock_us+=1000; edge(); acquisition_cycle(a);
    cm_acq_sample sample; CHECK(cm_acq_peek(&a->core,0,&sample) && sample.index==0);
    CHECK(cm_acq_idf_measured_rate_millihz(a)==1000000);
    (void)cm_acq_consume(&a->core,1);
    /* An edge arriving while the two-byte transaction is in flight invalidates
     * its value, preserving a gap rather than publishing a mislabeled sample. */
    interrupt_in_read=true; clock_us+=1000;edge();acquisition_cycle(a);
    CHECK(cm_acq_count(&a->core)==0);
    acquisition_cycle(a); CHECK(cm_acq_peek(&a->core,0,&sample) && sample.index==2);
    (void)cm_acq_consume(&a->core,1);
    level_high=false;unsigned before=reads;edge();acquisition_cycle(a);
    CHECK(reads==before && cm_acq_count(&a->core)==0);level_high=true;
    io_failure=true;edge();acquisition_cycle(a);
    cm_acq_snapshot(&a->core,&c);CHECK(c.i2c_failures==1 && c.inferred_missed_intervals==3);
    edge();edge();CHECK(cm_acq_idf_stop(a)==ESP_OK);
    CHECK(!irq_enabled && !cm_acq_idf_measurement_ready(a) && !cm_acq_idf_publishing(a));
    cm_acq_snapshot(&a->core,&c);CHECK(c.inferred_missed_intervals==5);
    CHECK(cm_acq_count(&a->core)==0);
    CHECK(cm_acq_idf_configure(a,CM_ACQ_POLICY_MEDIUM,CMQ_X,4,CM_ACQ_MODE_SAFE)==ESP_OK);
    CHECK(cm_acq_idf_prepare(a)==ESP_OK && cm_acq_idf_measured_rate_millihz(a)==0);
    CHECK(device.config.axis==CMQ_X && device.config.range_g==4 && device.config.mode==CMQ_MEDIUM);
    CHECK(cm_acq_idf_publish(a)==ESP_ERR_INVALID_STATE);
    uint32_t words[8]={0};
    CHECK(cm_acq_idf_zero_rate_probe(a,words)==ESP_ERR_INVALID_STATE && probes==0);
    clock_us+=300000;acquisition_cycle(a);
    CHECK(cm_acq_idf_zero_rate_probe(a,words)==ESP_OK && probes==2);
    CHECK(words[0]==0x01010101 && words[5]==0x02020202);
    CHECK(words[6]==0x10001 && words[7]==2);
    cm_acq_idf_destroy(a);
    CHECK(cm_test_critical==0);
    puts("CM-006 production IDF adapter scheduler tests passed");return 0;
}
