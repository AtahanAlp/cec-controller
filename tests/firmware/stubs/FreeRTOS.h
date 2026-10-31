#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef uint32_t TickType_t, StackType_t;
typedef void *TaskHandle_t;
typedef struct { int unused; } StaticTask_t, StaticMessageBuffer_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(n) (n)
#define configMAX_PRIORITIES 5
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
enum { eSetBits, eIncrement, eSetValueWithOverwrite };
#endif
