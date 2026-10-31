#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cec-frame.h"
#include "message_buffer.h"
#include "../../src/cec-log.c"
#define CHECK(x) do { if (!(x)) abort(); } while (0)
static unsigned logged;
uint64_t util_uptime_ms(void) { return 0; }
TaskHandle_t xTaskCreateStatic(void (*fn)(void *),const char *name,unsigned depth,void *context,unsigned priority,StackType_t *stack,StaticTask_t *tcb) {
  (void)fn;(void)name;(void)depth;(void)context;(void)priority;(void)stack;(void)tcb;return NULL;
}
size_t xMessageBufferReceive(MessageBufferHandle_t h,void *out,size_t size,TickType_t ticks) { (void)h;(void)out;(void)size;(void)ticks;return 0; }
MessageBufferHandle_t xMessageBufferCreateStatic(size_t n,uint8_t *data,StaticMessageBuffer_t *storage) { (void)n;(void)data;return storage; }
size_t xMessageBufferSend(MessageBufferHandle_t h,const void *data,size_t n,TickType_t ticks) {
  (void)h;CHECK(ticks==0);CHECK(n<=128 && memchr(data,0,n));logged++;return n;
}
int main(void) {
  cec_log_init(NULL);
  cec_message_t message={.data={0x04,0x0f},.len=2};cec_frame_t frame={.message=&message};
  cec_log_frame(&frame,true);CHECK(!logged);cec_log_enable();
  for (unsigned opcode=0;opcode<256;opcode++) {
    message.opcode=(uint8_t)opcode;message.len=16;memset(message.operand,255,sizeof(message.operand));
    cec_log_frame(&frame,true);cec_log_frame(&frame,false);
  }
  CHECK(logged==512);message.len=17;cec_log_frame(&frame,true);CHECK(logged==512);
  puts("All opcode and malformed logger regressions passed");return 0;
}
