#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
/* Exercise the actual ISR and alarm callbacks with a deterministic clock/GPIO. */
#include "../../src/cec-frame.c"
static uint64_t clock_us;
static bool output_low, external_low, enabled_irq, fail_alarm, lose_arbitration, broadcast_error;
static unsigned nack_byte;
static uint32_t notifications[2];
static void (*gpio_callback)(uint,uint32_t);
typedef struct { bool active; uint64_t at; alarm_callback_t callback; void *context; } alarm_t;
static alarm_t alarms[16];
uint64_t time_us_64(void) { return clock_us; }
static bool physical(void) { return !(output_low || external_low); }
static void changed(bool previous) {
  if (enabled_irq && previous != physical() && gpio_callback)
    gpio_callback(CEC_PIN, physical() ? GPIO_IRQ_EDGE_RISE : GPIO_IRQ_EDGE_FALL);
}
void gpio_set_dir(uint pin,bool out) { (void)pin; bool old=physical();output_low=out;changed(old); }
bool gpio_get(uint pin) {
  (void)pin;
  if (tx.active && tx.state==TX_DATA_SAMPLE && lose_arbitration) return false;
  if (tx.active && tx.state==TX_ACK_SAMPLE) {
    if ((tx.message.header&15)==15) return !broadcast_error;
    return tx.byte==nack_byte;
  }
  return physical();
}
void gpio_put(uint pin,bool value) { (void)pin;CHECK(!value); }
void gpio_init(uint pin) { (void)pin; }
void gpio_disable_pulls(uint pin) { (void)pin; }
void gpio_set_irq_enabled(uint pin,uint32_t events,bool enabled) { (void)pin;(void)events;enabled_irq=enabled; }
void gpio_acknowledge_irq(uint pin,uint32_t events) { (void)pin;(void)events; }
void gpio_set_irq_callback(void (*callback)(uint,uint32_t)) { gpio_callback=callback; }
void irq_set_enabled(uint irq,bool enabled) { (void)irq;(void)enabled; }
alarm_id_t add_alarm_at(absolute_time_t at,alarm_callback_t callback,void *context,bool past) {
  (void)past;
  if (fail_alarm) return -1;
  for (unsigned i=1;i<16;i++) if (!alarms[i].active) {
    alarms[i]=(alarm_t){.active=true,.at=at,.callback=callback,.context=context};return (alarm_id_t)i;
  }
  return -1;
}
bool cancel_alarm(alarm_id_t id) { if (id<=0 || id>=16) return false;alarms[id].active=false;return true; }
static unsigned next_alarm(uint64_t end) {
  unsigned selected=0;
  for (unsigned i=1;i<16;i++)
    if (alarms[i].active && alarms[i].at<=end && (!selected || alarms[i].at<alarms[selected].at)) selected=i;
  return selected;
}
static void run_one(unsigned index) {
  alarm_t alarm=alarms[index];alarms[index].active=false;clock_us=alarm.at;
  int64_t repeat=alarm.callback((alarm_id_t)index,alarm.context);
  if (repeat) {
    CHECK(repeat>0 && repeat<10000);
    alarms[index]=alarm;alarms[index].active=true;alarms[index].at=clock_us+(uint64_t)repeat;
  }
}
static void run_to(uint64_t end) {
  unsigned count=0,index;
  while ((index=next_alarm(end))) { CHECK(++count<10000);run_one(index); }
  CHECK(clock_us<=end);clock_us=end;
}
void vTaskDelay(TickType_t ticks) { run_to(clock_us+(uint64_t)ticks*1000); }
BaseType_t xTaskNotifyWaitIndexed(UBaseType_t index,uint32_t enter,uint32_t leave,uint32_t *value,TickType_t wait) {
  (void)enter;
  if (!notifications[index] && wait) run_to(clock_us+(uint64_t)wait*1000);
  *value=notifications[index];notifications[index]&=~leave;return *value ? pdTRUE:pdFALSE;
}
uint32_t ulTaskNotifyTakeIndexed(UBaseType_t index,BaseType_t clear,TickType_t wait) {
  (void)clear;
  uint64_t end=clock_us+(uint64_t)wait*1000;
  unsigned alarm;
  while (!notifications[index] && (alarm=next_alarm(end))) run_one(alarm);
  if (!notifications[index]) clock_us=end;
  uint32_t value=notifications[index];notifications[index]=0;return value;
}
BaseType_t xTaskNotifyIndexedFromISR(TaskHandle_t task,UBaseType_t index,uint32_t value,int mode,BaseType_t *wake) {
  (void)task;(void)wake;
  if (mode==eIncrement) notifications[index]++;
  else notifications[index]|=value;
  return pdTRUE;
}
void cec_log_frame(cec_frame_t *frame,bool recv) { (void)frame;(void)recv; }
static void reset(void) {
  memset(alarms,0,sizeof(alarms));memset(notifications,0,sizeof(notifications));memset(&stats,0,sizeof(stats));memset(&tx,0,sizeof(tx));
  output_low=external_low=enabled_irq=fail_alarm=lose_arbitration=broadcast_error=false;
  nack_byte=0;clock_us=0;ack_alarm=-1;cec_frame_init();address=4;run_to(20000);
}
static void edge(uint64_t at,bool low) { run_to(at);bool old=physical();external_low=low;changed(old); }
static void receive(const uint8_t *data,unsigned length) {
  uint64_t at=clock_us+20000;
  edge(at,true);edge(at+3700,false);at+=4500;
  for (unsigned byte=0;byte<length;byte++) {
    for (int bit=7;bit>=0;bit--) {
      edge(at,true);edge(at+((data[byte]&(1u<<bit))?600:1500),false);at+=2400;
    }
    edge(at,true);edge(at+(byte+1==length?600:1500),false);at+=2400;
    edge(at,true);edge(at+600,false);at+=2400;run_to(at);
  }
}
static void rx_regressions(void) {
  reset();uint8_t report[]={0x04,0x90,0};receive(report,sizeof(report));
  notifications[0]|=NOTIFY_RX_TX;cec_message_t message={0};cec_frame_recv(&message,4);
  CHECK(message.len==3 && !memcmp(message.data,report,3));CHECK(!output_low);
  reset();uint8_t oversized[17]={0x04,0xa0};receive(oversized,sizeof(oversized));
  CHECK(rx.message.len<=16 && stats.rx_abort_frames);cec_frame_recv(&message,4);CHECK(!message.len);
  receive(report,sizeof(report));cec_frame_recv(&message,4);CHECK(message.len==3);
  reset();edge(clock_us+20000,true);edge(clock_us+3700,false);run_to(clock_us+10000);
  cec_frame_recv(&message,4);CHECK(rx.state==RX_START && !output_low);
  reset();uint8_t truncated[]={0x04,0x80};receive(truncated,sizeof(truncated));cec_frame_recv(&message,4);CHECK(!message.len);
  reset();fail_alarm=true;receive(report,sizeof(report));CHECK(!output_low && stats.rx_abort_frames);
}
static void tx_regressions(void) {
  cec_message_t message={.data={0x40,0x04},.len=2};
  reset();CHECK(cec_frame_send_timeout(&message,1000)==CEC_TX_ACK);CHECK(!output_low && enabled_irq);
  reset();nack_byte=1;CHECK(cec_frame_send_timeout(&message,1000)==CEC_TX_NO_ACK);CHECK(tx.byte==1 && !output_low);
  reset();nack_byte=2;CHECK(cec_frame_send_timeout(&message,1000)==CEC_TX_NO_ACK);CHECK(tx.byte==2);
  reset();lose_arbitration=true;CHECK(cec_frame_send_timeout(&message,1000)==CEC_TX_ARBITRATION_LOST);CHECK(!output_low && enabled_irq);
  reset();message.header=0x4f;CHECK(cec_frame_send_timeout(&message,1000)==CEC_TX_ACK);
  reset();broadcast_error=true;CHECK(cec_frame_send_timeout(&message,1000)==CEC_TX_BUS_ERROR);
  reset();fail_alarm=true;CHECK(cec_frame_send_timeout(&message,1000)==CEC_TX_TIMEOUT);CHECK(!output_low && enabled_irq);
  reset();CHECK(cec_frame_send_timeout(&message,5)==CEC_TX_TIMEOUT);CHECK(clock_us<=25000 && !output_low);
  reset();external_low=true;CHECK(cec_frame_send_timeout(&message,50)==CEC_TX_TIMEOUT);CHECK(clock_us<=70000 && !output_low);
}
int main(void) { rx_regressions();tx_regressions();puts("CEC ISR, byte ACK, arbitration, and recovery regressions passed");return 0; }
