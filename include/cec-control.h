#ifndef CEC_CONTROL_H
#define CEC_CONTROL_H
#include <stdbool.h>
#include <stdint.h>
#include "cec-message.h"

#define CEC_CONTROL_PROTOCOL_VERSION 2
#define CEC_CONTROL_TX_TIMEOUT_MS 1500
#define CEC_CONTROL_POWER_TIMEOUT_MS 2000
#define CEC_CONTROL_ON_TIMEOUT_MS 30000
#define CEC_CONTROL_STANDBY_TIMEOUT_MS 10000
#define CEC_CONTROL_STATUS_TIMEOUT_MS 3000
#define CEC_CONTROL_POLL_MS 500
#define CEC_CONTROL_RETRY_DELAY_MS 2000
#define CEC_CONTROL_ON_ATTEMPTS 3
#define CEC_CONTROL_STANDBY_ATTEMPTS 2

typedef enum {
  CEC_POWER_QUERY_OK, CEC_POWER_QUERY_NO_ACK, CEC_POWER_QUERY_TX_TIMEOUT,
  CEC_POWER_QUERY_RESPONSE_TIMEOUT, CEC_POWER_QUERY_UNAVAILABLE, CEC_POWER_QUERY_UNSUPPORTED,
} cec_power_query_result_t;

typedef struct {
  void *context;
  cec_tx_result_t (*send)(void *, const cec_message_t *, uint32_t);
  void (*delay)(void *, uint32_t);
  cec_power_query_result_t (*query_power)(void *, uint8_t, uint8_t *, uint32_t);
  uint64_t (*now_ms)(void *);
  bool (*routing_confirmed)(void *, uint16_t);
  uint64_t deadline_ms;
} cec_control_bus_t;

typedef enum {
  CEC_CONTROL_OK, CEC_CONTROL_INVALID_PHYSICAL_ADDRESS, CEC_CONTROL_NO_ACK,
  CEC_CONTROL_TX_TIMEOUT, CEC_CONTROL_RESPONSE_TIMEOUT, CEC_CONTROL_UNAVAILABLE,
  CEC_CONTROL_TRANSITION_TIMEOUT, CEC_CONTROL_UNSUPPORTED, CEC_CONTROL_INPUT_UNCONFIRMED,
} cec_control_result_code_t;

typedef struct {
  cec_control_result_code_t code;
  cec_tx_result_t tx_result;
  uint8_t attempts, power_status;
  bool power_confirmed, source_announced, routing_confirmed;
  uint32_t elapsed_ms;
  const char *phase;
} cec_control_result_t;

bool cec_control_valid_physical_address(uint16_t address);
cec_control_result_t cec_control_on(const cec_control_bus_t *, uint8_t, uint16_t, uint8_t);
cec_control_result_t cec_control_standby(const cec_control_bus_t *, uint8_t);
cec_control_result_t cec_control_status(const cec_control_bus_t *, uint8_t);
const char *cec_control_result_name(cec_control_result_code_t);
const char *cec_control_power_name(uint8_t);
#endif
