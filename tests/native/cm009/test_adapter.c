/* Run actual application callbacks and CDC pump, replacing external devices,
 * scheduler and stack calls. This is not USB enumeration or hardware evidence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../firmware-usb/components/contactmic_usb/cm_usb_idf.c"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static cm_acq_idf acq;
static bool frontend,cdc_open=true,usb_mounted=true;
static uint32_t cdc_room=64;
static unsigned backend_calls,usb_init_calls,phy_calls,task_calls;
static uint64_t clock_us;
static esp_err_t configure_error, prepare_error;
static uint32_t measured_rate = 1000000;
static int drdy_level;
int gpio_get_level(gpio_num_t pin) {CHECK(!frontend&&pin==13);return drdy_level;}
static char input[2048],captured[8192];
static size_t input_at,input_length,captured_length;
static uint8_t last_audio[96];
static void feed(const char *text)
{ input_length=strlen(text);CHECK(input_length<sizeof(input));memcpy(input,text,input_length);input_at=0; }
static void io_guard(void) { CHECK(!frontend);++backend_calls; }
cmq_result cmq_reset(cmq_device *d) {io_guard();d->state=CMQ_IDLE;return CMQ_OK;}
esp_err_t cm_acq_idf_create(cmq_device *d,cm_acq_idf **out)
{CHECK(d);*out=&acq;return ESP_OK;}
esp_err_t cm_acq_idf_configure(cm_acq_idf *a,cm_acq_policy policy,cmq_axis axis,uint8_t range,cm_acq_mode ceiling)
{io_guard();CHECK(a==&acq&&policy<=CM_ACQ_POLICY_HIGH&&axis<=CMQ_Z&&(range==2||range==4)&&ceiling==CM_ACQ_MODE_SAFE);return configure_error;}
esp_err_t cm_acq_idf_prepare(cm_acq_idf *a) {io_guard();a->ready=true;return prepare_error;}
esp_err_t cm_acq_idf_publish(cm_acq_idf *a) {io_guard();a->core.head=a->core.tail=a->core.next=0;a->live=true;return ESP_OK;}
esp_err_t cm_acq_idf_stop(cm_acq_idf *a) {io_guard();a->live=a->ready=false;a->core.head=a->core.tail=0;return ESP_OK;}
bool cm_acq_idf_measurement_ready(const cm_acq_idf *a){return a->ready;}
bool cm_acq_idf_publishing(const cm_acq_idf *a){return a->live;}
uint32_t cm_acq_idf_measured_rate_millihz(const cm_acq_idf *a){(void)a;return measured_rate;}
cm_acq *cm_acq_idf_core(cm_acq_idf *a){return &a->core;}
size_t cm_acq_count(const cm_acq *a){return a->head-a->tail;}
bool cm_acq_peek(const cm_acq *a,size_t n,cm_acq_sample *out)
{if(n>=cm_acq_count(a))return false;*out=a->samples[(a->tail+n)%256];return true;}
size_t cm_acq_consume(cm_acq *a,size_t n){if(n>cm_acq_count(a))n=cm_acq_count(a);a->tail+=(uint32_t)n;return n;}
bool cm_acq_try_snapshot(const cm_acq *a,cm_acq_counters *out){*out=a->counters;return true;}
esp_err_t usb_new_phy(const usb_phy_config_t *p,usb_phy_handle_t *out)
{CHECK(p->controller==USB_PHY_CTRL_OTG&&p->target==USB_PHY_TARGET_INT&&p->otg_mode==USB_OTG_MODE_DEVICE&&p->otg_speed==USB_PHY_SPEED_FULL);++phy_calls;*out=&acq;return ESP_OK;}
esp_err_t esp_efuse_mac_get_default(uint8_t *m){memset(m,0xAB,6);return ESP_OK;}
/* IDF's helper is capped by CONFIG_APP_RETRIEVE_LEN_ELF_SHA (default nine). */
int esp_app_get_elf_sha256(char *out,size_t n){CHECK(n>=10);memset(out,'a',9);out[9]=0;return 10;}
const esp_app_desc_t *esp_app_get_description(void)
{static esp_app_desc_t app;for(unsigned i=0;i<32;i++)app.app_elf_sha256[i]=(uint8_t)i;return &app;}
uint32_t esp_random(void){return 42;}
int esp_reset_reason(void){return 1;}
int64_t esp_timer_get_time(void){return (int64_t)clock_us;}
BaseType_t xTaskCreate(void (*fn)(void *),const char *name,uint32_t stack,void *arg,unsigned priority,TaskHandle_t *out)
{(void)fn;(void)name;(void)arg;CHECK((stack==4096&&priority==5)||(stack==6144&&priority==8));CHECK(!out);++task_calls;return pdPASS;}
void vTaskDelay(TickType_t t){CHECK(t==10);}
bool tusb_init(uint8_t port,const tusb_rhport_init_t *p){CHECK(port==0&&p->role==TUSB_ROLE_DEVICE&&p->speed==TUSB_SPEED_FULL);++usb_init_calls;return true;}
bool tud_disconnect(void){usb_mounted=false;return true;}
bool tud_connect(void){return true;}
bool tud_mounted(void){return usb_mounted;}
bool tud_audio_clear_ep_in_ff(void){memset(last_audio,0,96);return true;}
uint16_t tud_audio_write(const void *p,uint16_t n){CHECK(n==96);memcpy(last_audio,p,n);return n;}
bool tud_audio_buffer_and_schedule_control_xfer(uint8_t port,const tusb_control_request_t *r,void *p,uint16_t n)
{(void)r;CHECK(port==0&&p&&n<=14);return true;}
bool tud_cdc_connected(void){return cdc_open;}
uint32_t tud_cdc_read(void *p,uint32_t size)
{CHECK(size==64);size_t n=input_length-input_at;if(n>size)n=size;memcpy(p,input+input_at,n);input_at+=n;return (uint32_t)n;}
uint32_t tud_cdc_write_available(void){return cdc_room;}
uint32_t tud_cdc_write(const void *p,uint32_t n)
{CHECK(n<=64&&n<=cdc_room&&captured_length+n<sizeof(captured));memcpy(captured+captured_length,p,n);captured_length+=n;captured[captured_length]=0;return n;}
uint32_t tud_cdc_write_flush(void){return 0;}
void tud_cdc_read_flush(void){input_at=input_length;}
bool tud_cdc_write_clear(void){return true;}
void tud_task_ext(uint32_t ms,bool isr){CHECK(ms==1&&!isr);}
static void produce(unsigned n)
{for(unsigned i=0;i<n;i++){cm_acq *a=&acq.core;CHECK(cm_acq_count(a)<256);a->samples[(a->head++)%256]=(cm_acq_sample){a->next++,1000};++a->counters.drdy_edges;++a->counters.successful_reads;}}
int main(void)
{
    cmq_device d={.who_am_i=5,.address=106,.revision=104};
    CHECK(cmu_idf_start(&d,true)==ESP_OK);
    CHECK(phy_calls==1&&usb_init_calls==1&&task_calls==2);
    CHECK(cmu_idf_start(&d,true)==ESP_ERR_INVALID_STATE);
    CHECK(!strcmp(runtime.serial,"ABABABABABAB"));
    CHECK(strlen(runtime.elf_hash)==64);
    CHECK(!strcmp(runtime.elf_hash,"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"));
    CHECK(tud_descriptor_string_cb(3,0x0409)[0]==0x031a);
    CHECK(!tud_descriptor_string_cb(9,0x0409));
    tud_mount_cb();
    tusb_control_request_t open={1,11,1,1,0}, close={1,11,0,1,0};
    frontend=true;
    CHECK(!tud_audio_tx_done_pre_load_cb(0,1,0x81,1));
    CHECK(!tud_audio_tx_done_pre_load_cb(0,0,0x83,1));
    CHECK(!tud_audio_tx_done_pre_load_cb(0,0,0x81,0));
    /* Pinned TinyUSB audio_device.c passes idx_audio_fct=0, not AS interface 1. */
    CHECK(tud_audio_tx_done_pre_load_cb(0,0,0x81,1));
    uint32_t generation=runtime.core.generation;
    CHECK(tud_audio_set_itf_cb(0,&open));
    CHECK(runtime.core.generation==generation&&backend_calls==0);
    for(unsigned i=0;i<96;i++)CHECK(!last_audio[i]);
    frontend=false;
    cmu_worker_step(&runtime.core,0);cmu_worker_step(&runtime.core,250000);
    cmu_tick(&runtime.core,250000);produce(24);
    clock_us=251000;frontend=true;
    CHECK(tud_audio_tx_done_pre_load_cb(0,0,0x81,1));
    CHECK(runtime.core.state==CMU_RUNNING);
    unsigned calls=backend_calls;
    feed("STATUS 7\n");cdc_room=0;tud_cdc_rx_cb(0);
    CHECK(runtime.tx_used>1000&&runtime.tx_sent==0);
    CHECK(strstr(runtime.tx,"\"request_id\":7")&&strstr(runtime.tx,"\"source_snapshot_fresh\":true"));
    /* Flooding/closed CDC must not stall source or output callbacks. */
    feed("STATUS 8\nSTATUS 9\n");cdc_pump();
    CHECK(runtime.core.counters.cdc_dropped_replies==2);
    ++clock_us;CHECK(tud_audio_tx_done_pre_load_cb(0,0,0x81,1));
    CHECK(backend_calls==calls);
    cdc_room=64;
    for(unsigned i=0;i<60;i++)tud_cdc_tx_complete_cb(0);
    CHECK(strstr(captured,"\"serial\":\"ABABABABABAB\""));
    memset(input,'X',200);input[200]='\n';input_length=201;input_at=0;
    for(unsigned i=0;i<4;i++)cdc_pump();
    CHECK(runtime.core.counters.cdc_overflows==1);
    cdc_open=false;tud_cdc_line_state_cb(0,false,false);CHECK(!runtime.tx_used);
    CHECK(runtime.core.requested); /* DTR never owns audio lifetime. */
    cdc_open=true;
    /* An old transfer callback after a bus-reset indication cannot restart. */
    tud_event_hook_cb(0,DCD_EVENT_BUS_RESET,true);
    CHECK(!tud_audio_tx_done_pre_load_cb(0,0,0x81,1));
    CHECK(!runtime.core.requested&&!runtime.mounted);
    tud_mount_cb();CHECK(tud_audio_set_itf_cb(0,&open));
    CHECK(runtime.core.generation>generation);
    CHECK(tud_audio_set_itf_close_EP_cb(0,&close));
    CHECK(!runtime.core.requested);
    tud_suspend_cb(false);CHECK(runtime.reconnect_pending&&!runtime.mounted);
    CHECK(!tud_audio_tx_done_pre_load_cb(0,0,0x81,1));
    CHECK(backend_calls==calls); /* All the above callbacks remained nonblocking. */
    frontend=false;cmu_worker_step(&runtime.core,300000);
    CHECK(!acq.live);
    /* Distinguish real configure/start failures from zero-edge measurement,
     * retaining the last failure across successful cleanup/status queries. */
    configure_error=ESP_FAIL;
    CHECK(source_begin(NULL,&runtime.core.config,0)==CMU_IO);
    CHECK(runtime.backend_failure==((2u<<16)|0xffffu));
    configure_error=ESP_OK;prepare_error=ESP_FAIL;
    CHECK(source_begin(NULL,&runtime.core.config,0)==CMU_IO);
    CHECK(runtime.backend_failure==((3u<<16)|0xffffu));
    prepare_error=ESP_OK;CHECK(source_begin(NULL,&runtime.core.config,0)==CMU_OK);
    measured_rate=0;uint32_t rate;drdy_level=1;
    CHECK(source_ready(NULL,&rate)==CMU_IO);
    CHECK(runtime.drdy_levels_seen==2);
    drdy_level=0;CHECK(source_ready(NULL,&rate)==CMU_IO);
    CHECK(runtime.drdy_levels_seen==3);
    CHECK(runtime.backend_failure==((4u<<16)|0xffffu));
    CHECK(source_stop(NULL)==CMU_OK);
    CHECK(runtime.backend_failure==((4u<<16)|0xffffu));
    puts("CM-009 real USB adapter callback/CDC isolation tests passed");return 0;
}
