#ifndef TEST_MESSAGE_BUFFER_H
#define TEST_MESSAGE_BUFFER_H
#include "FreeRTOS.h"
typedef void *MessageBufferHandle_t;
size_t xMessageBufferReceive(MessageBufferHandle_t, void *, size_t, TickType_t);
MessageBufferHandle_t xMessageBufferCreateStatic(size_t, uint8_t *, StaticMessageBuffer_t *);
size_t xMessageBufferSend(MessageBufferHandle_t, const void *, size_t, TickType_t);
#endif
