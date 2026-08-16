#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cec-control.h"
#include "cec-id.h"

#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))
#define CHECK(condition)                                                              \
  do {                                                                                \
    if (!(condition)) {                                                               \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      exit(1);                                                                        \
    }                                                                                 \
  } while (0)

typedef struct {
  cec_tx_result_t tx_results[8];
  size_t tx_result_count;
  size_t tx_result_index;
  cec_message_t messages[8];
  size_t message_count;
  uint32_t delays[8];
  size_t delay_count;
  cec_power_query_result_t query_result;
  uint8_t query_status;
  uint8_t query_initiator;
  uint32_t query_timeout;
} mock_bus_t;

static cec_tx_result_t mock_send(void *context,
                                 const cec_message_t *message,
                                 uint32_t timeout_ms) {
  mock_bus_t *mock = context;
  CHECK(timeout_ms == CEC_CONTROL_TX_TIMEOUT_MS);
  CHECK(mock->message_count < ARRAY_LEN(mock->messages));
  mock->messages[mock->message_count++] = *message;
  CHECK(mock->tx_result_index < mock->tx_result_count);
  return mock->tx_results[mock->tx_result_index++];
}

static void mock_delay(void *context, uint32_t delay_ms) {
  mock_bus_t *mock = context;
  CHECK(mock->delay_count < ARRAY_LEN(mock->delays));
  mock->delays[mock->delay_count++] = delay_ms;
}

static cec_power_query_result_t mock_query(void *context,
                                           uint8_t initiator,
                                           uint8_t *power_status,
                                           uint32_t timeout_ms) {
  mock_bus_t *mock = context;
  mock->query_initiator = initiator;
  mock->query_timeout = timeout_ms;
  *power_status = mock->query_status;
  return mock->query_result;
}

static cec_control_bus_t make_bus(mock_bus_t *mock) {
  return (cec_control_bus_t){
      .context = mock,
      .send = mock_send,
      .delay = mock_delay,
      .query_power = mock_query,
  };
}

static void test_physical_address_validation(void) {
  CHECK(cec_control_valid_physical_address(0x1000));
  CHECK(cec_control_valid_physical_address(0x1200));
  CHECK(cec_control_valid_physical_address(0x1234));
  CHECK(!cec_control_valid_physical_address(0x0000));
  CHECK(!cec_control_valid_physical_address(0xffff));
  CHECK(!cec_control_valid_physical_address(0x0100));
  CHECK(!cec_control_valid_physical_address(0x1010));
}

static void test_on_success(void) {
  mock_bus_t mock = {
      .tx_results = {CEC_TX_NO_ACK, CEC_TX_ACK, CEC_TX_NO_ACK},
      .tx_result_count = 3,
  };
  cec_control_bus_t bus = make_bus(&mock);
  cec_control_result_t result = cec_control_on(&bus, 0x04, 0x1200, 0x04);

  CHECK(result.code == CEC_CONTROL_OK);
  CHECK(result.attempts == 1);
  CHECK(mock.message_count == 3);
  CHECK(mock.messages[0].header == 0x4f);
  CHECK(mock.messages[0].opcode == CEC_ID_REPORT_PHYSICAL_ADDRESS);
  CHECK(mock.messages[0].operand[0] == 0x12);
  CHECK(mock.messages[0].operand[1] == 0x00);
  CHECK(mock.messages[0].operand[2] == 0x04);
  CHECK(mock.messages[1].header == 0x40);
  CHECK(mock.messages[1].opcode == CEC_ID_IMAGE_VIEW_ON);
  CHECK(mock.messages[2].header == 0x4f);
  CHECK(mock.messages[2].opcode == CEC_ID_ACTIVE_SOURCE);
  CHECK(mock.delay_count == 1);
  CHECK(mock.delays[0] == CEC_CONTROL_WAKE_DELAY_MS);
}

static void test_on_retries_and_reports_no_ack(void) {
  mock_bus_t mock = {
      .tx_results = {
          CEC_TX_NO_ACK,
          CEC_TX_NO_ACK,
          CEC_TX_NO_ACK,
          CEC_TX_NO_ACK,
          CEC_TX_NO_ACK,
      },
      .tx_result_count = 5,
  };
  cec_control_bus_t bus = make_bus(&mock);
  cec_control_result_t result = cec_control_on(&bus, 0x08, 0x2000, 0x04);

  CHECK(result.code == CEC_CONTROL_NO_ACK);
  CHECK(result.attempts == CEC_CONTROL_ON_ATTEMPTS);
  CHECK(mock.message_count == 5);
  CHECK(mock.messages[4].opcode == CEC_ID_ACTIVE_SOURCE);
  CHECK(mock.delay_count == 3);
  CHECK(mock.delays[0] == CEC_CONTROL_RETRY_DELAY_MS);
  CHECK(mock.delays[1] == CEC_CONTROL_RETRY_DELAY_MS);
  CHECK(mock.delays[2] == CEC_CONTROL_WAKE_DELAY_MS);
}

static void test_on_rejects_invalid_address(void) {
  mock_bus_t mock = {0};
  cec_control_bus_t bus = make_bus(&mock);
  cec_control_result_t result = cec_control_on(&bus, 0x04, 0x0000, 0x04);

  CHECK(result.code == CEC_CONTROL_INVALID_PHYSICAL_ADDRESS);
  CHECK(mock.message_count == 0);

  result = cec_control_on(&bus, 0x0f, 0x1000, 0x04);
  CHECK(result.code == CEC_CONTROL_UNAVAILABLE);
  CHECK(mock.message_count == 0);
}

static void test_on_stops_after_broadcast_timeout(void) {
  mock_bus_t mock = {
      .tx_results = {CEC_TX_TIMEOUT},
      .tx_result_count = 1,
  };
  cec_control_bus_t bus = make_bus(&mock);
  cec_control_result_t result = cec_control_on(&bus, 0x04, 0x1000, 0x04);

  CHECK(result.code == CEC_CONTROL_TX_TIMEOUT);
  CHECK(result.attempts == 0);
  CHECK(mock.message_count == 1);
  CHECK(mock.delay_count == 0);
}

static void test_standby_retry(void) {
  mock_bus_t mock = {
      .tx_results = {CEC_TX_NO_ACK, CEC_TX_ACK},
      .tx_result_count = 2,
  };
  cec_control_bus_t bus = make_bus(&mock);
  cec_control_result_t result = cec_control_standby(&bus, 0x0b);

  CHECK(result.code == CEC_CONTROL_OK);
  CHECK(result.attempts == 2);
  CHECK(mock.message_count == 2);
  CHECK(mock.messages[0].header == 0xb0);
  CHECK(mock.messages[0].opcode == CEC_ID_STANDBY);
  CHECK(mock.delay_count == 1);
  CHECK(mock.delays[0] == CEC_CONTROL_RETRY_DELAY_MS);
}

static void test_status(void) {
  mock_bus_t mock = {
      .query_result = CEC_POWER_QUERY_OK,
      .query_status = 0x01,
  };
  cec_control_bus_t bus = make_bus(&mock);
  cec_control_result_t result = cec_control_status(&bus, 0x08);

  CHECK(result.code == CEC_CONTROL_OK);
  CHECK(result.power_status == 0x01);
  CHECK(mock.query_initiator == 0x08);
  CHECK(mock.query_timeout == CEC_CONTROL_POWER_TIMEOUT_MS);
  CHECK(strcmp(cec_control_power_name(result.power_status), "standby") == 0);

  mock.query_result = CEC_POWER_QUERY_RESPONSE_TIMEOUT;
  result = cec_control_status(&bus, 0x08);
  CHECK(result.code == CEC_CONTROL_RESPONSE_TIMEOUT);
}

int main(void) {
  test_physical_address_validation();
  test_on_success();
  test_on_retries_and_reports_no_ack();
  test_on_rejects_invalid_address();
  test_on_stops_after_broadcast_timeout();
  test_standby_retry();
  test_status();
  puts("cec-control tests passed");
  return 0;
}
