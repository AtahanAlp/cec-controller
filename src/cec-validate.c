#include <stddef.h>
#include "cec-validate.h"
#include "cec-id.h"

bool cec_physical_address_valid(uint16_t address) {
  if (address == 0 || address == 0xffff) return false;
  bool zero = false;
  for (int shift = 12; shift >= 0; shift -= 4) {
    unsigned nibble = (address >> shift) & 15;
    if (zero && nibble) return false;
    if (!nibble) zero = true;
  }
  return true;
}

bool cec_message_valid(const cec_message_t *message) {
  if (!message || !message->len || message->len > CEC_FRAME_MAX_LEN) return false;
  if (message->len == 1) return true;
  unsigned minimum = 2;
  switch (message->opcode) {
    case CEC_ID_ROUTING_CHANGE: minimum = 6; break;
    case CEC_ID_REPORT_PHYSICAL_ADDRESS:
    case CEC_ID_DEVICE_VENDOR_ID:
    case CEC_ID_VENDOR_COMMAND_WITH_ID: minimum = 5; break;
    case CEC_ID_FEATURE_ABORT:
    case CEC_ID_ROUTING_INFORMATION:
    case CEC_ID_ACTIVE_SOURCE:
    case CEC_ID_INACTIVE_SOURCE:
    case CEC_ID_SET_STREAM_PATH: minimum = 4; break;
    case CEC_ID_REPORT_POWER_STATUS:
    case CEC_ID_USER_CONTROL_PRESSED:
    case CEC_ID_SET_SYSTEM_AUDIO_MODE:
    case CEC_ID_SYSTEM_AUDIO_MODE_STATUS:
    case CEC_ID_REPORT_AUDIO_STATUS:
    case CEC_ID_MENU_REQUEST:
    case CEC_ID_MENU_STATUS:
    case CEC_ID_CEC_VERSION: minimum = 3; break;
    default: break;
  }
  return message->len >= minimum;
}
