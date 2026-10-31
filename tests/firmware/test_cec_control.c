#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cec-control.h"
#include "cec-id.h"
#include "cec-validate.h"
#include "cec-config.h"
#include "cec-health.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
typedef struct {
  uint64_t now;
  unsigned sends, queries, power_sends, active_sends;
  uint8_t opcodes[32];
  uint8_t status[64]; unsigned status_count, index;
  cec_power_query_result_t query_result;
  cec_tx_result_t send_result;
  bool route;
  uint64_t target_at, first_active_at, slow_boot_at;
} mock_t;
static uint64_t now(void *p) { return ((mock_t *)p)->now; }
static void delay(void *p, uint32_t duration) { ((mock_t *)p)->now += duration; }
static cec_tx_result_t send(void *p, const cec_message_t *message, uint32_t timeout) {
  mock_t *m=p; CHECK(timeout && timeout<=1500); CHECK(cec_message_valid(message));
  CHECK(m->sends < 32); m->opcodes[m->sends++]=message->opcode;
  if (message->opcode==CEC_ID_IMAGE_VIEW_ON || message->opcode==CEC_ID_STANDBY) m->power_sends++;
  if (message->opcode==CEC_ID_ACTIVE_SOURCE) {
    CHECK(m->queries>=2); if (!m->active_sends++) m->first_active_at=m->now;
    CHECK(m->now>=m->target_at);
  }
  m->now += timeout < 50 ? timeout : 50;
  return m->send_result;
}
static cec_power_query_result_t query(void *p,uint8_t address,uint8_t *status,uint32_t timeout) {
  mock_t *m=p; CHECK(address>0 && address<15); CHECK(timeout && timeout<=2000);
  m->queries++; m->now += timeout<50 ? timeout : 50;
  unsigned index=m->index<m->status_count ? m->index++ : (m->status_count ? m->status_count-1:0);
  *status=m->slow_boot_at ? (m->now < 13000 ? 1 : m->now < m->slow_boot_at ? 2 : 0) : m->status[index];
  return m->query_result;
}
static bool route(void *p,uint16_t address) { CHECK(address==0x3000); return ((mock_t *)p)->route; }
static cec_control_bus_t bus(mock_t *m) {
  return (cec_control_bus_t){.context=m,.send=send,.delay=delay,.query_power=query,.now_ms=now,.routing_confirmed=route};
}
static mock_t normal(void) { return (mock_t){.send_result=CEC_TX_ACK,.query_result=CEC_POWER_QUERY_OK,.route=true}; }
static void wake_waits_for_on(void) {
  mock_t m=normal(); uint8_t states[]={3,3,1,2,2,2,0,0};
  memcpy(m.status,states,sizeof(states));m.status_count=sizeof(states);m.target_at=3000;
  cec_control_bus_t b=bus(&m);cec_control_result_t r=cec_control_on(&b,4,0x3000,4);
  CHECK(r.code==CEC_CONTROL_OK && r.power_confirmed && r.routing_confirmed);
  CHECK(m.opcodes[0]==CEC_ID_IMAGE_VIEW_ON);CHECK(m.first_active_at>=m.target_at);
  CHECK(m.queries>=8 && r.elapsed_ms<20000);
}
static void slow_tv_boot(void) {
  mock_t m=normal();m.slow_boot_at=22000;m.target_at=22000;cec_control_bus_t b=bus(&m);
  cec_control_result_t r=cec_control_on(&b,4,0x3000,4);
  CHECK(r.code==CEC_CONTROL_OK && r.power_confirmed && r.elapsed_ms>22000 && r.elapsed_ms<30000);
}
static void standby_waits_then_wake(void) {
  mock_t m=normal();uint8_t states[]={0,3,3,1,1};memcpy(m.status,states,sizeof(states));m.status_count=sizeof(states);
  cec_control_bus_t b=bus(&m);cec_control_result_t off=cec_control_standby(&b,4);
  CHECK(off.code==CEC_CONTROL_OK && off.power_confirmed && m.queries==5);
  uint64_t finished=m.now; m.status[0]=2;m.status[1]=2;m.status[2]=0;m.status[3]=0;m.status_count=4;m.index=0;
  cec_control_result_t on=cec_control_on(&b,4,0x3000,4);
  CHECK(on.code==CEC_CONTROL_OK && m.now>finished);
}
static void ack_is_not_completion(void) {
  mock_t m=normal();m.status[0]=2;m.status_count=1;cec_control_bus_t b=bus(&m);
  cec_control_result_t r=cec_control_on(&b,4,0x3000,4);
  CHECK(r.code==CEC_CONTROL_TRANSITION_TIMEOUT && !r.power_confirmed && !m.active_sends);
  CHECK(r.elapsed_ms==30000 && m.power_sends==1);
}
static void missing_feedback_is_failure(void) {
  mock_t m=normal();m.query_result=CEC_POWER_QUERY_RESPONSE_TIMEOUT;cec_control_bus_t b=bus(&m);
  cec_control_result_t r=cec_control_standby(&b,4);
  CHECK(r.code==CEC_CONTROL_RESPONSE_TIMEOUT && !r.power_confirmed && r.elapsed_ms==10000);
  CHECK(m.power_sends==2);
  m=normal();m.query_result=CEC_POWER_QUERY_UNSUPPORTED;b=bus(&m);
  r=cec_control_on(&b,4,0x3000,4);CHECK(r.code==CEC_CONTROL_UNSUPPORTED && m.queries==1);
}
static void partial_routing_is_explicit(void) {
  mock_t m=normal();m.route=false;cec_control_bus_t b=bus(&m);
  cec_control_result_t r=cec_control_on(&b,4,0x3000,4);
  CHECK(r.code==CEC_CONTROL_INPUT_UNCONFIRMED && r.power_confirmed && r.source_announced && !r.routing_confirmed);
  CHECK(m.active_sends==2);
}
static void limits_and_invalid_inputs(void) {
  CHECK(cec_health_fresh(5000, 0, 0, 0));
  CHECK(cec_health_fresh(20000, 0, 19000, 18000));
  CHECK(!cec_health_fresh(20000, 0, 10000, 19000));
  CHECK(!cec_health_fresh(20000, 0, 19000, 10000));
  CHECK(cec_health_fresh(100, UINT32_MAX - 20000, UINT32_MAX - 10, UINT32_MAX - 10));
  mock_t m=normal();cec_control_bus_t b=bus(&m);
  CHECK(cec_control_on(&b,4,0xffff,4).code==CEC_CONTROL_INVALID_PHYSICAL_ADDRESS);
  CHECK(cec_control_on(&b,15,0x3000,4).code==CEC_CONTROL_UNAVAILABLE);CHECK(!m.sends);
  m.status[0]=2;m.status_count=1;b.deadline_ms=100;
  cec_control_result_t r=cec_control_on(&b,4,0x3000,4);CHECK(r.elapsed_ms<=100 && r.code!=CEC_CONTROL_OK);
  m=normal();m.status[0]=255;m.status_count=1;b=bus(&m);
  CHECK(cec_control_status(&b,4).code!=CEC_CONTROL_OK);
  CHECK(!cec_physical_address_valid(0xffff) && !cec_physical_address_valid(0x1010));
  cec_config_t c={0};cec_config_set_default(&c);CHECK(cec_config_valid(&c));
  c.device_type=255;CHECK(!cec_config_valid(&c));c.device_type=4;memset(c.osd_name,'x',sizeof(c.osd_name));CHECK(!cec_config_valid(&c));
  cec_message_t msg={.len=17};CHECK(!cec_message_valid(&msg));msg.len=2;msg.opcode=CEC_ID_ROUTING_CHANGE;CHECK(!cec_message_valid(&msg));
}
int main(void) {
  wake_waits_for_on();slow_tv_boot();standby_waits_then_wake();ack_is_not_completion();missing_feedback_is_failure();
  partial_routing_is_explicit();limits_and_invalid_inputs();puts("Confirmed power transition regressions passed");return 0;
}
