#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../src/cec-task.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
TaskHandle_t xCECTask=(TaskHandle_t)(uintptr_t)1;
static uint64_t clock_us, arrival_us;
static uint32_t pending[2];
static unsigned wire_sends, mode, waits;
static cec_message_t last_sent;
uint64_t time_us_64(void) { return clock_us; }
uint64_t cec_frame_received_at(void) { return arrival_us; }
uint64_t cec_frame_sent_at(void) { return clock_us - 1000; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)(uintptr_t)2; }
void vTaskDelay(TickType_t ticks) { clock_us+=(uint64_t)ticks*1000; }
BaseType_t xTaskNotifyIndexed(TaskHandle_t task,UBaseType_t index,uint32_t value,int action) {
  (void)action;if (task!=xCECTask) pending[index]=value;return pdTRUE;
}
BaseType_t xTaskNotifyWaitIndexed(UBaseType_t index,uint32_t enter,uint32_t leave,uint32_t *value,TickType_t wait) {
  (void)enter;(void)leave;
  if (wait && index==0 && !pending[index]) {
    if ((mode==1 || mode==3) && !waits++) {
      pending[index]=((tx_generation-1)<<8)|CEC_TX_NO_ACK;clock_us+=1000;
    } else if (mode==0 || mode==1) service_tx_request();
  }
  if (wait && !pending[index]) clock_us+=(uint64_t)wait*1000;
  *value=pending[index];pending[index]=0;return *value?pdTRUE:pdFALSE;
}
cec_tx_result_t cec_frame_send_timeout(const cec_message_t *message,uint32_t timeout) {
  CHECK(timeout);wire_sends++;last_sent=*message;clock_us+=1000;return CEC_TX_ACK;
}
cec_tx_result_t cec_frame_send(const cec_message_t *message) {
  wire_sends++;last_sent=*message;
  return message->len==1 && (message->header&15)!=4 ? CEC_TX_NO_ACK:CEC_TX_ACK;
}
bool cec_frame_idle(void) { return true; }
void cec_frame_init(void) {}
void cec_frame_recv(cec_message_t *message,uint8_t address) { (void)address;message->len=0; }
void cec_log_submitf(const char *format,...) { (void)format; }
uint16_t ddc_get_physical_address(void) { return 0; }
void nvs_load_config(cec_config_t *value) { cec_config_set_default(value); }
void blink_set(blink_state_t value) { (void)value; }
void blink_set_blink(blink_state_t value) { (void)value; }
static void reset(void) {
  cec_tx_queue=xQueueCreateStatic(CEC_TX_QUEUE_LEN,sizeof(cec_tx_request_t),cec_tx_queue_storage,&cec_tx_queue_buf);
  memset(pending,0,sizeof(pending));tx_generation=100;cancelled_tx=0;clock_us=100000;arrival_us=clock_us;
  wire_sends=mode=waits=0;laddr=4;paddr=0x3000;source_active=false;power_status_waiter=NULL;
  cec_config_set_default(&config);config.physical_address=0x3000;
}
static void generations_and_expiry(void) {
  cec_message_t message={.data={0x40,0x04},.len=2};
  reset();CHECK(cec_send_msg_sync(&message,50)==CEC_TX_ACK && wire_sends==1);
  reset();mode=2;CHECK(cec_send_msg_sync(&message,50)==CEC_TX_TIMEOUT);
  CHECK(!wire_sends && cec_tx_queue->count==1);
  CHECK(service_tx_request() && !wire_sends); /* expired/cancelled request cannot transmit later */
  mode=1;waits=0;CHECK(cec_send_msg_sync(&message,50)==CEC_TX_ACK && wire_sends==1);
  reset();mode=3;CHECK(cec_send_msg_sync(&message,50)==CEC_TX_TIMEOUT && !wire_sends);
  CHECK(service_tx_request() && !wire_sends);
}
static void feedback_and_passive_bus(void) {
  reset();power_status_waiter=xTaskGetCurrentTaskHandle();power_query_since=clock_us;power_query_deadline=clock_us+100000;
  cec_message_t report={.data={0x04,CEC_ID_REPORT_POWER_STATUS,0},.len=3};
  arrival_us=clock_us-1;dispatch_message(report);CHECK(power_status_waiter && !pending[1]);
  arrival_us=clock_us;dispatch_message(report);CHECK(!power_status_waiter && pending[1]==1);
  power_status_waiter=xTaskGetCurrentTaskHandle();pending[1]=0;report.operand[0]=255;dispatch_message(report);CHECK(!pending[1]);
  report.opcode=CEC_ID_FEATURE_ABORT;report.operand[0]=CEC_ID_GIVE_DEVICE_POWER_STATUS;report.operand[1]=4;report.len=4;
  dispatch_message(report);CHECK(pending[1]==0x104);
  reset();cec_message_t request={.data={0x0f,CEC_ID_REQUEST_ACTIVE_SOURCE},.len=2};
  for (unsigned i=0;i<10;i++) dispatch_message(request);
  CHECK(!wire_sends && !source_active);cec_set_source_active(true);dispatch_message(request);
  CHECK(wire_sends==1 && last_sent.opcode==CEC_ID_ACTIVE_SOURCE);
  request.opcode=CEC_ID_STANDBY;dispatch_message(request);request.opcode=CEC_ID_REQUEST_ACTIVE_SOURCE;dispatch_message(request);
  CHECK(wire_sends==1 && !source_active);
  request.header=0x4f;dispatch_message(request);CHECK(laddr==15); /* competing initiator invalidates our address */
}
static void routing_evidence(void) {
  reset();cec_message_t route={.data={0x0f,CEC_ID_ROUTING_CHANGE,0,0,0x30,0},.len=6};
  dispatch_message(route);
  CHECK(cec_routing_confirmed(0x3000,arrival_us));
  CHECK(!cec_routing_confirmed(0x3000,arrival_us+1));
  cec_set_source_active(true);
  CHECK(cec_routing_confirmed(0x3000,arrival_us)); /* fresh TV routing may precede final activation */
  route.header=0x8f;route.opcode=CEC_ID_ACTIVE_SOURCE;route.operand[0]=0x20;route.operand[1]=0;route.len=4;
  dispatch_message(route);cec_set_source_active(true);
  CHECK(!cec_routing_confirmed(0x3000,arrival_us)); /* another source invalidates the evidence */
}
static void allocation_and_query_budget(void) {
  reset();CHECK(allocate_logical_address(&config)==8); /* 4 occupied, 8 free */
  reset();uint8_t power;uint64_t began=clock_us;
  CHECK(cec_query_power_status(&power,100)==CEC_POWER_QUERY_RESPONSE_TIMEOUT);
  CHECK(clock_us-began<=100000);
}
int main(void) { generations_and_expiry();feedback_and_passive_bus();routing_evidence();allocation_and_query_budget();puts("CEC request expiry, stale notification, feedback, and source ownership regressions passed");return 0; }
