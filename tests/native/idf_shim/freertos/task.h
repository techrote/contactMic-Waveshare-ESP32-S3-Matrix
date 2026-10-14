#ifndef CM_TEST_TASK_H
#define CM_TEST_TASK_H
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(void (*fn)(void *), const char *, uint32_t, void *, unsigned, TaskHandle_t *);
void vTaskDelete(TaskHandle_t);
void vTaskNotifyGiveFromISR(TaskHandle_t, BaseType_t *);
uint32_t ulTaskNotifyValueClear(TaskHandle_t, uint32_t);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
void xTaskNotifyGive(TaskHandle_t);
#endif
