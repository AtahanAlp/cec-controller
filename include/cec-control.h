#ifndef CEC_CONTROL_H
#define CEC_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "cec-message.h"

#define CEC_CONTROL_PROTOCOL_VERSION (1)
#define CEC_CONTROL_TX_TIMEOUT_MS (1500)
#define CEC_CONTROL_POWER_TIMEOUT_MS (2000)
#define CEC_CONTROL_WAKE_DELAY_MS (1500)
#define CEC_CONTROL_RETRY_DELAY_MS (200)
#define CEC_CONTROL_ON_ATTEMPTS (3)
#define CEC_CONTROL_STANDBY_ATTEMPTS (2)

typedef enum {
  CEC_POWER_QUERY_OK = 0,
  CEC_POWER_QUERY_NO_ACK,
  CEC_POWER_QUERY_TX_TIMEOUT,
  CEC_POWER_QUERY_RESPONSE_TIMEOUT,
  CEC_POWER_QUERY_UNAVAILABLE,
} cec_power_query_result_t;

typedef cec_tx_result_t (*cec_control_send_fn)(void *context,
                                               const cec_message_t *message,
                                               uint32_t timeout_ms);
typedef void (*cec_control_delay_fn)(void *context, uint32_t delay_ms);
typedef cec_power_query_result_t (*cec_control_query_power_fn)(void *context,
                                                               uint8_t initiator,
                                                               uint8_t *power_status,
                                                               uint32_t timeout_ms);

typedef struct {
  void *context;
  cec_control_send_fn send;
  cec_control_delay_fn delay;
  cec_control_query_power_fn query_power;
} cec_control_bus_t;

typedef enum {
  CEC_CONTROL_OK = 0,
  CEC_CONTROL_INVALID_PHYSICAL_ADDRESS,
  CEC_CONTROL_NO_ACK,
  CEC_CONTROL_TX_TIMEOUT,
  CEC_CONTROL_RESPONSE_TIMEOUT,
  CEC_CONTROL_UNAVAILABLE,
} cec_control_result_code_t;

typedef struct {
  cec_control_result_code_t code;
  cec_tx_result_t tx_result;
  uint8_t attempts;
  uint8_t power_status;
} cec_control_result_t;

bool cec_control_valid_physical_address(uint16_t physical_address);
cec_control_result_t cec_control_on(const cec_control_bus_t *bus,
                                    uint8_t logical_address,
                                    uint16_t physical_address,
                                    uint8_t device_type);
cec_control_result_t cec_control_standby(const cec_control_bus_t *bus,
                                         uint8_t logical_address);
cec_control_result_t cec_control_status(const cec_control_bus_t *bus,
                                        uint8_t logical_address);
const char *cec_control_result_name(cec_control_result_code_t code);
const char *cec_control_power_name(uint8_t power_status);

#endif
