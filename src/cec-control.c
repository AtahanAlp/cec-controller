#include <stddef.h>

#include "cec-control.h"

#include "cec-id.h"

static bool tx_completed(cec_tx_result_t result) {
  return result == CEC_TX_ACK || result == CEC_TX_NO_ACK;
}

static bool valid_logical_address(uint8_t logical_address) {
  return logical_address != 0x00 && logical_address < 0x0f;
}

static cec_control_result_code_t tx_failure_code(cec_tx_result_t result) {
  return result == CEC_TX_TIMEOUT ? CEC_CONTROL_TX_TIMEOUT : CEC_CONTROL_UNAVAILABLE;
}

static cec_control_result_t make_result(cec_control_result_code_t code,
                                        cec_tx_result_t tx_result,
                                        uint8_t attempts) {
  return (cec_control_result_t){
      .code = code,
      .tx_result = tx_result,
      .attempts = attempts,
      .power_status = 0xff,
  };
}

bool cec_control_valid_physical_address(uint16_t physical_address) {
  if (physical_address == 0x0000 || physical_address == 0xffff) {
    return false;
  }

  bool zero_seen = false;
  for (int shift = 12; shift >= 0; shift -= 4) {
    uint8_t nibble = (physical_address >> shift) & 0x0f;
    if (zero_seen && nibble != 0) {
      return false;
    }
    if (nibble == 0) {
      zero_seen = true;
    }
  }
  return true;
}

cec_control_result_t cec_control_on(const cec_control_bus_t *bus,
                                    uint8_t logical_address,
                                    uint16_t physical_address,
                                    uint8_t device_type) {
  if (bus == NULL || bus->send == NULL || bus->delay == NULL) {
    return make_result(CEC_CONTROL_UNAVAILABLE, CEC_TX_UNAVAILABLE, 0);
  }
  if (!valid_logical_address(logical_address)) {
    return make_result(CEC_CONTROL_UNAVAILABLE, CEC_TX_UNAVAILABLE, 0);
  }
  if (!cec_control_valid_physical_address(physical_address)) {
    return make_result(CEC_CONTROL_INVALID_PHYSICAL_ADDRESS, CEC_TX_UNAVAILABLE, 0);
  }

  cec_message_t report = {
      .header = HEADER0(logical_address, 0x0f),
      .opcode = CEC_ID_REPORT_PHYSICAL_ADDRESS,
      .operand = {(physical_address >> 8) & 0xff, physical_address & 0xff, device_type},
      .len = 5,
  };
  cec_tx_result_t tx = bus->send(bus->context, &report, CEC_CONTROL_TX_TIMEOUT_MS);
  if (!tx_completed(tx)) {
    return make_result(tx_failure_code(tx), tx, 0);
  }

  cec_message_t wake = {
      .header = HEADER0(logical_address, 0x00),
      .opcode = CEC_ID_IMAGE_VIEW_ON,
      .len = 2,
  };
  uint8_t attempts = 0;
  cec_tx_result_t wake_tx = CEC_TX_UNAVAILABLE;
  for (attempts = 1; attempts <= CEC_CONTROL_ON_ATTEMPTS; attempts++) {
    wake_tx = bus->send(bus->context, &wake, CEC_CONTROL_TX_TIMEOUT_MS);
    if (wake_tx == CEC_TX_ACK) {
      break;
    }
    if (wake_tx == CEC_TX_UNAVAILABLE) {
      return make_result(CEC_CONTROL_UNAVAILABLE, wake_tx, attempts);
    }
    if (attempts < CEC_CONTROL_ON_ATTEMPTS) {
      bus->delay(bus->context, CEC_CONTROL_RETRY_DELAY_MS);
    }
  }
  if (attempts > CEC_CONTROL_ON_ATTEMPTS) {
    attempts = CEC_CONTROL_ON_ATTEMPTS;
  }

  bus->delay(bus->context, CEC_CONTROL_WAKE_DELAY_MS);

  cec_message_t active = {
      .header = HEADER0(logical_address, 0x0f),
      .opcode = CEC_ID_ACTIVE_SOURCE,
      .operand = {(physical_address >> 8) & 0xff, physical_address & 0xff},
      .len = 4,
  };
  tx = bus->send(bus->context, &active, CEC_CONTROL_TX_TIMEOUT_MS);
  if (!tx_completed(tx)) {
    return make_result(tx_failure_code(tx), tx, attempts);
  }
  if (wake_tx != CEC_TX_ACK) {
    cec_control_result_code_t code = wake_tx == CEC_TX_TIMEOUT ? CEC_CONTROL_TX_TIMEOUT
                                                               : CEC_CONTROL_NO_ACK;
    if (wake_tx == CEC_TX_UNAVAILABLE) {
      code = CEC_CONTROL_UNAVAILABLE;
    }
    return make_result(code, wake_tx, attempts);
  }

  return make_result(CEC_CONTROL_OK, wake_tx, attempts);
}

cec_control_result_t cec_control_standby(const cec_control_bus_t *bus,
                                         uint8_t logical_address) {
  if (bus == NULL || bus->send == NULL || bus->delay == NULL) {
    return make_result(CEC_CONTROL_UNAVAILABLE, CEC_TX_UNAVAILABLE, 0);
  }
  if (!valid_logical_address(logical_address)) {
    return make_result(CEC_CONTROL_UNAVAILABLE, CEC_TX_UNAVAILABLE, 0);
  }

  cec_message_t standby = {
      .header = HEADER0(logical_address, 0x00),
      .opcode = CEC_ID_STANDBY,
      .len = 2,
  };
  cec_tx_result_t tx = CEC_TX_UNAVAILABLE;
  uint8_t attempts = 0;
  for (attempts = 1; attempts <= CEC_CONTROL_STANDBY_ATTEMPTS; attempts++) {
    tx = bus->send(bus->context, &standby, CEC_CONTROL_TX_TIMEOUT_MS);
    if (tx == CEC_TX_ACK) {
      return make_result(CEC_CONTROL_OK, tx, attempts);
    }
    if (tx == CEC_TX_UNAVAILABLE) {
      break;
    }
    if (attempts < CEC_CONTROL_STANDBY_ATTEMPTS) {
      bus->delay(bus->context, CEC_CONTROL_RETRY_DELAY_MS);
    }
  }
  if (attempts > CEC_CONTROL_STANDBY_ATTEMPTS) {
    attempts = CEC_CONTROL_STANDBY_ATTEMPTS;
  }

  cec_control_result_code_t code = CEC_CONTROL_NO_ACK;
  if (tx == CEC_TX_TIMEOUT) {
    code = CEC_CONTROL_TX_TIMEOUT;
  } else if (tx == CEC_TX_UNAVAILABLE) {
    code = CEC_CONTROL_UNAVAILABLE;
  }
  return make_result(code, tx, attempts);
}

cec_control_result_t cec_control_status(const cec_control_bus_t *bus,
                                        uint8_t logical_address) {
  if (bus == NULL || bus->query_power == NULL) {
    return make_result(CEC_CONTROL_UNAVAILABLE, CEC_TX_UNAVAILABLE, 0);
  }
  if (!valid_logical_address(logical_address)) {
    return make_result(CEC_CONTROL_UNAVAILABLE, CEC_TX_UNAVAILABLE, 0);
  }

  uint8_t power_status = 0xff;
  cec_power_query_result_t query =
      bus->query_power(bus->context, logical_address, &power_status, CEC_CONTROL_POWER_TIMEOUT_MS);
  cec_control_result_t result = make_result(CEC_CONTROL_OK, CEC_TX_ACK, 1);
  result.power_status = power_status;

  switch (query) {
    case CEC_POWER_QUERY_OK:
      break;
    case CEC_POWER_QUERY_NO_ACK:
      result.code = CEC_CONTROL_NO_ACK;
      result.tx_result = CEC_TX_NO_ACK;
      break;
    case CEC_POWER_QUERY_TX_TIMEOUT:
      result.code = CEC_CONTROL_TX_TIMEOUT;
      result.tx_result = CEC_TX_TIMEOUT;
      break;
    case CEC_POWER_QUERY_RESPONSE_TIMEOUT:
      result.code = CEC_CONTROL_RESPONSE_TIMEOUT;
      break;
    case CEC_POWER_QUERY_UNAVAILABLE:
      result.code = CEC_CONTROL_UNAVAILABLE;
      result.tx_result = CEC_TX_UNAVAILABLE;
      break;
  }
  return result;
}

const char *cec_control_result_name(cec_control_result_code_t code) {
  switch (code) {
    case CEC_CONTROL_OK:
      return "ok";
    case CEC_CONTROL_INVALID_PHYSICAL_ADDRESS:
      return "invalid_physical_address";
    case CEC_CONTROL_NO_ACK:
      return "no_ack";
    case CEC_CONTROL_TX_TIMEOUT:
      return "tx_timeout";
    case CEC_CONTROL_RESPONSE_TIMEOUT:
      return "response_timeout";
    case CEC_CONTROL_UNAVAILABLE:
    default:
      return "unavailable";
  }
}

const char *cec_control_power_name(uint8_t power_status) {
  switch (power_status) {
    case 0x00:
      return "on";
    case 0x01:
      return "standby";
    case 0x02:
      return "transitioning_on";
    case 0x03:
      return "transitioning_standby";
    default:
      return "unknown";
  }
}
