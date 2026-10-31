#include <stddef.h>
#include "cec-control.h"
#include "cec-id.h"
#include "cec-validate.h"

bool cec_control_valid_physical_address(uint16_t address) { return cec_physical_address_valid(address); }

static cec_control_result_t initial_result(void) {
  return (cec_control_result_t){ .code = CEC_CONTROL_UNAVAILABLE,
    .tx_result = CEC_TX_UNAVAILABLE, .power_status = 255, .phase = "ready" };
}
static bool available(const cec_control_bus_t *bus, uint8_t address) {
  return bus && bus->send && bus->delay && bus->query_power && bus->now_ms && address > 0 && address < 15;
}
static uint32_t remaining(const cec_control_bus_t *bus, uint64_t deadline, uint32_t maximum) {
  uint64_t now = bus->now_ms(bus->context);
  if (now >= deadline) return 0;
  return deadline - now < maximum ? (uint32_t)(deadline - now) : maximum;
}
static uint64_t operation_deadline(const cec_control_bus_t *bus, uint32_t timeout) {
  uint64_t deadline = bus->now_ms(bus->context) + timeout;
  return bus->deadline_ms && bus->deadline_ms < deadline ? bus->deadline_ms : deadline;
}
static void pause_operation(const cec_control_bus_t *bus, uint64_t deadline, uint32_t duration) {
  duration = remaining(bus, deadline, duration);
  if (duration) bus->delay(bus->context, duration);
}
static cec_control_result_t finish(const cec_control_bus_t *bus, uint64_t started,
                                    cec_control_result_t result) {
  result.elapsed_ms = (uint32_t)(bus->now_ms(bus->context) - started);
  return result;
}
static cec_control_result_code_t query_error(cec_power_query_result_t query) {
  switch (query) {
    case CEC_POWER_QUERY_OK: return CEC_CONTROL_OK;
    case CEC_POWER_QUERY_NO_ACK: return CEC_CONTROL_NO_ACK;
    case CEC_POWER_QUERY_TX_TIMEOUT: return CEC_CONTROL_TX_TIMEOUT;
    case CEC_POWER_QUERY_RESPONSE_TIMEOUT: return CEC_CONTROL_RESPONSE_TIMEOUT;
    case CEC_POWER_QUERY_UNSUPPORTED: return CEC_CONTROL_UNSUPPORTED;
    default: return CEC_CONTROL_UNAVAILABLE;
  }
}

static cec_control_result_t confirm_power(const cec_control_bus_t *bus, uint8_t address,
                                          uint8_t target, uint64_t deadline) {
  cec_control_result_t result = initial_result();
  result.phase = "power";
  uint64_t next_send = 0;
  unsigned stable = 0;
  bool any_report = false;
  unsigned maximum = target == 0 ? CEC_CONTROL_ON_ATTEMPTS : CEC_CONTROL_STANDBY_ATTEMPTS;
  cec_message_t command = { .header = HEADER0(address, 0),
    .opcode = target == 0 ? CEC_ID_IMAGE_VIEW_ON : CEC_ID_STANDBY, .len = 2 };
  while (remaining(bus, deadline, 1)) {
    bool transition = result.power_status == 2 || result.power_status == 3;
    if (bus->now_ms(bus->context) >= next_send && result.attempts < maximum
        && !transition && result.power_status != target) {
      uint32_t timeout = remaining(bus, deadline, CEC_CONTROL_TX_TIMEOUT_MS);
      result.tx_result = bus->send(bus->context, &command, timeout);
      result.attempts++;
      next_send = bus->now_ms(bus->context) + CEC_CONTROL_RETRY_DELAY_MS;
      if (result.tx_result == CEC_TX_UNAVAILABLE) { result.code = CEC_CONTROL_UNAVAILABLE; return result; }
    }
    uint32_t timeout = remaining(bus, deadline, CEC_CONTROL_POWER_TIMEOUT_MS);
    if (!timeout) break;
    uint8_t status = 255;
    cec_power_query_result_t query = bus->query_power(bus->context, address, &status, timeout);
    result.code = query_error(query);
    if (query == CEC_POWER_QUERY_UNSUPPORTED || query == CEC_POWER_QUERY_UNAVAILABLE) return result;
    if (query == CEC_POWER_QUERY_OK && status <= 3) {
      any_report = true; result.power_status = status;
      stable = status == target ? stable + 1 : 0;
      if (stable == 2) {
        result.code = CEC_CONTROL_OK; result.power_confirmed = true; return result;
      }
    } else stable = 0;
    pause_operation(bus, deadline, CEC_CONTROL_POLL_MS);
  }
  result.code = any_report ? CEC_CONTROL_TRANSITION_TIMEOUT
             : result.code == CEC_CONTROL_NO_ACK ? CEC_CONTROL_NO_ACK : CEC_CONTROL_RESPONSE_TIMEOUT;
  return result;
}

cec_control_result_t cec_control_on(const cec_control_bus_t *bus, uint8_t address,
                                    uint16_t physical, uint8_t type) {
  cec_control_result_t result = initial_result();
  if (!available(bus, address)) return result;
  if (!cec_physical_address_valid(physical)) { result.code = CEC_CONTROL_INVALID_PHYSICAL_ADDRESS; return result; }
  uint64_t started = bus->now_ms(bus->context);
  uint64_t deadline = operation_deadline(bus, CEC_CONTROL_ON_TIMEOUT_MS);
  /* Wake first. Several TVs reject wake sequences preceded by other control messages. */
  result = confirm_power(bus, address, 0, deadline);
  if (result.code != CEC_CONTROL_OK) return finish(bus, started, result);
  result.phase = "activate";
  pause_operation(bus, deadline, CEC_CONTROL_POLL_MS);
  cec_message_t report = { .header = HEADER0(address, 15), .opcode = CEC_ID_REPORT_PHYSICAL_ADDRESS,
    .operand = { physical >> 8, physical & 255, type }, .len = 5 };
  uint32_t timeout = remaining(bus, deadline, CEC_CONTROL_TX_TIMEOUT_MS);
  if (!timeout || bus->send(bus->context, &report, timeout) != CEC_TX_ACK) {
    result.code = CEC_CONTROL_INPUT_UNCONFIRMED; return finish(bus, started, result);
  }
  cec_message_t active = { .header = HEADER0(address, 15), .opcode = CEC_ID_ACTIVE_SOURCE,
    .operand = { physical >> 8, physical & 255 }, .len = 4 };
  for (unsigned attempt = 0; attempt < 2; attempt++) {
    timeout = remaining(bus, deadline, CEC_CONTROL_TX_TIMEOUT_MS);
    if (!timeout) break;
    result.tx_result = bus->send(bus->context, &active, timeout);
    if (result.tx_result != CEC_TX_ACK) break;
    result.source_announced = true;
    pause_operation(bus, deadline, CEC_CONTROL_POLL_MS);
    if (bus->routing_confirmed && bus->routing_confirmed(bus->context, physical)) {
      result.routing_confirmed = true; result.code = CEC_CONTROL_OK;
      return finish(bus, started, result);
    }
  }
  result.code = CEC_CONTROL_INPUT_UNCONFIRMED;
  return finish(bus, started, result);
}

cec_control_result_t cec_control_standby(const cec_control_bus_t *bus, uint8_t address) {
  cec_control_result_t result = initial_result();
  if (!available(bus, address)) return result;
  uint64_t started = bus->now_ms(bus->context);
  return finish(bus, started, confirm_power(bus, address, 1,
    operation_deadline(bus, CEC_CONTROL_STANDBY_TIMEOUT_MS)));
}

cec_control_result_t cec_control_status(const cec_control_bus_t *bus, uint8_t address) {
  cec_control_result_t result = initial_result();
  if (!available(bus, address)) return result;
  uint64_t started = bus->now_ms(bus->context);
  uint64_t deadline = operation_deadline(bus, CEC_CONTROL_STATUS_TIMEOUT_MS);
  uint32_t timeout = remaining(bus, deadline, CEC_CONTROL_POWER_TIMEOUT_MS);
  if (!timeout) { result.code = CEC_CONTROL_RESPONSE_TIMEOUT; return finish(bus, started, result); }
  result.phase = "status"; result.attempts = 1;
  cec_power_query_result_t query = bus->query_power(bus->context, address, &result.power_status, timeout);
  result.code = query_error(query);
  if (result.code == CEC_CONTROL_OK && result.power_status > 3) result.code = CEC_CONTROL_RESPONSE_TIMEOUT;
  result.power_confirmed = result.code == CEC_CONTROL_OK;
  return finish(bus, started, result);
}

const char *cec_control_result_name(cec_control_result_code_t code) {
  switch (code) {
    case CEC_CONTROL_OK: return "ok";
    case CEC_CONTROL_INVALID_PHYSICAL_ADDRESS: return "invalid_physical_address";
    case CEC_CONTROL_NO_ACK: return "no_ack";
    case CEC_CONTROL_TX_TIMEOUT: return "tx_timeout";
    case CEC_CONTROL_RESPONSE_TIMEOUT: return "response_timeout";
    case CEC_CONTROL_TRANSITION_TIMEOUT: return "transition_timeout";
    case CEC_CONTROL_UNSUPPORTED: return "unsupported_feedback";
    case CEC_CONTROL_INPUT_UNCONFIRMED: return "input_unconfirmed";
    default: return "unavailable";
  }
}
const char *cec_control_power_name(uint8_t status) {
  switch (status) {
    case 0: return "on";
    case 1: return "standby";
    case 2: return "transitioning_on";
    case 3: return "transitioning_standby";
    default: return "unknown";
  }
}
