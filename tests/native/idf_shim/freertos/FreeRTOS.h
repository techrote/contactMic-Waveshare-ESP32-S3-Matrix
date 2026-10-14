#ifndef CM_TEST_FREERTOS_H
#define CM_TEST_FREERTOS_H
#include <stdint.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef unsigned portMUX_TYPE;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define configMAX_PRIORITIES 25
#define pdMS_TO_TICKS(x) ((x) / 10u)
extern unsigned cm_test_critical;
#define portMUX_INITIALIZE(p) (*(p)=0)
#define portENTER_CRITICAL(p) ((void)(p), ++cm_test_critical)
#define portEXIT_CRITICAL(p) ((void)(p), --cm_test_critical)
#define portENTER_CRITICAL_ISR(p) portENTER_CRITICAL(p)
#define portEXIT_CRITICAL_ISR(p) portEXIT_CRITICAL(p)
#define portYIELD_FROM_ISR() ((void)0)
#endif
