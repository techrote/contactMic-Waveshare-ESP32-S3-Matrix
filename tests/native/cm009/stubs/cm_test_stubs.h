#ifndef CM_TEST_STUBS_H
#define CM_TEST_STUBS_H
/* Explicit native substitutes for external IDF/TinyUSB/QMI APIs. The actual
 * firmware CI separately compiles the same adapter against pinned real headers. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NO_MEM 2
typedef enum {CMQ_OK, CMQ_IO} cmq_result;
enum {CMQ_IDLE,CMQ_FAULT};
typedef enum {CMQ_X,CMQ_Y,CMQ_Z} cmq_axis;
typedef struct {int state; uint8_t who_am_i,address,revision;} cmq_device;
cmq_result cmq_reset(cmq_device *);
cmq_result cmq_set_int2_output(cmq_device *,bool);
typedef enum {CM_ACQ_POLICY_SAFE,CM_ACQ_POLICY_MEDIUM,CM_ACQ_POLICY_HIGH} cm_acq_policy;
typedef enum {CM_ACQ_MODE_SAFE,CM_ACQ_MODE_MEDIUM,CM_ACQ_MODE_HIGH} cm_acq_mode;
typedef struct {uint32_t index; int16_t value;} cm_acq_sample;
typedef struct {uint64_t drdy_edges,successful_reads,inferred_missed_intervals,i2c_failures,source_faults,ring_overruns;} cm_acq_counters;
typedef struct {cm_acq_sample samples[256]; uint32_t head,tail,next; cm_acq_counters counters;} cm_acq;
typedef struct {cm_acq core; bool ready,live;} cm_acq_idf;
typedef struct {uint32_t pre_low,pre_pending,post_low,post_pending,read_calls,read_us_min,read_us_max,read_us_total;} cm_acq_idf_timing;
void cm_acq_idf_timing_snapshot(const cm_acq_idf *,cm_acq_idf_timing *);
esp_err_t cm_acq_idf_create(cmq_device *,cm_acq_idf **);
esp_err_t cm_acq_idf_configure(cm_acq_idf *,cm_acq_policy,cmq_axis,uint8_t,cm_acq_mode);
esp_err_t cm_acq_idf_prepare(cm_acq_idf *);
esp_err_t cm_acq_idf_publish(cm_acq_idf *);
esp_err_t cm_acq_idf_stop(cm_acq_idf *);
bool cm_acq_idf_measurement_ready(const cm_acq_idf *);
bool cm_acq_idf_publishing(const cm_acq_idf *);
uint32_t cm_acq_idf_measured_rate_millihz(const cm_acq_idf *);
cm_acq *cm_acq_idf_core(cm_acq_idf *);
esp_err_t cm_acq_idf_zero_rate_probe(cm_acq_idf *,uint32_t out[8]);
esp_err_t cm_acq_idf_pulse_probe(cm_acq_idf *,uint32_t out[6]);
bool cm_acq_peek(const cm_acq *,size_t,cm_acq_sample *);
size_t cm_acq_consume(cm_acq *,size_t);
size_t cm_acq_count(const cm_acq *);
bool cm_acq_try_snapshot(const cm_acq *,cm_acq_counters *);
typedef void *usb_phy_handle_t;
enum {USB_PHY_CTRL_OTG,USB_PHY_TARGET_INT,USB_OTG_MODE_DEVICE,USB_PHY_SPEED_FULL};
typedef struct {int controller,target,otg_mode,otg_speed;} usb_phy_config_t;
esp_err_t usb_new_phy(const usb_phy_config_t *,usb_phy_handle_t *);
esp_err_t esp_efuse_mac_get_default(uint8_t *);
int esp_app_get_elf_sha256(char *,size_t);
typedef struct {uint8_t app_elf_sha256[32];} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
uint32_t esp_random(void);
int esp_reset_reason(void);
int64_t esp_timer_get_time(void);
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
BaseType_t xTaskCreate(void (*)(void *),const char *,uint32_t,void *,unsigned,TaskHandle_t *);
void vTaskDelay(TickType_t);
#define CONFIG_CONTACTMIC_USB_AUTO_CEILING 0
#define TUSB_ROLE_DEVICE 1
#define TUSB_SPEED_FULL 1
#define DCD_EVENT_BUS_RESET 1
#define DCD_EVENT_UNPLUGGED 2
typedef struct {int role,speed;} tusb_rhport_init_t;
typedef struct {uint8_t bmRequestType,bRequest; uint16_t wValue,wIndex,wLength;} tusb_control_request_t;
bool tusb_init(uint8_t,const tusb_rhport_init_t *);
bool tud_disconnect(void);
bool tud_connect(void);
bool tud_mounted(void);
bool tud_audio_clear_ep_in_ff(void);
uint16_t tud_audio_write(const void *,uint16_t);
bool tud_audio_buffer_and_schedule_control_xfer(uint8_t,const tusb_control_request_t *,void *,uint16_t);
bool tud_cdc_connected(void);
uint32_t tud_cdc_read(void *,uint32_t);
uint32_t tud_cdc_write_available(void);
uint32_t tud_cdc_write(const void *,uint32_t);
uint32_t tud_cdc_write_flush(void);
void tud_cdc_read_flush(void);
bool tud_cdc_write_clear(void);
void tud_task_ext(uint32_t,bool);
#endif
