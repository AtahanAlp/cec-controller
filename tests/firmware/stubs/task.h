#ifndef TEST_TASK_H
#define TEST_TASK_H
#include "FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyIndexed(TaskHandle_t, UBaseType_t, uint32_t, int);
BaseType_t xTaskNotifyWaitIndexed(UBaseType_t, uint32_t, uint32_t, uint32_t *, TickType_t);
uint32_t ulTaskNotifyTakeIndexed(UBaseType_t, BaseType_t, TickType_t);
BaseType_t xTaskNotifyIndexedFromISR(TaskHandle_t, UBaseType_t, uint32_t, int, BaseType_t *);
TaskHandle_t xTaskCreateStatic(void (*)(void *), const char *, unsigned, void *, unsigned,
                              StackType_t *, StaticTask_t *);
#endif
