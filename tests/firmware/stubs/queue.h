#ifndef TEST_QUEUE_H
#define TEST_QUEUE_H
#include <string.h>
#include "FreeRTOS.h"
typedef struct { uint8_t *data; unsigned size, length, head, tail, count; } StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
static inline QueueHandle_t xQueueCreateStatic(unsigned n, unsigned size, uint8_t *data, StaticQueue_t *q) {
  *q = (StaticQueue_t){ .data = data, .size = size, .length = n }; return q;
}
static inline BaseType_t xQueueSend(QueueHandle_t q, const void *data, TickType_t ticks) {
  (void)ticks;
  if (q->count == q->length) return pdFALSE;
  memcpy(q->data + q->tail * q->size, data, q->size);
  q->tail = (q->tail + 1) % q->length; q->count++; return pdTRUE;
}
static inline BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *data, BaseType_t *wake) {
  (void)wake; return xQueueSend(q, data, 0);
}
static inline BaseType_t xQueueReceive(QueueHandle_t q, void *data, TickType_t ticks) {
  (void)ticks;
  if (!q->count) return pdFALSE;
  memcpy(data, q->data + q->head * q->size, q->size);
  q->head = (q->head + 1) % q->length; q->count--; return pdTRUE;
}
#endif
