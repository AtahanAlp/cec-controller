#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "pico/stdlib.h"

#include "blink.h"
#include "cec-config.h"
#include "cec-frame.h"
#include "cec-id.h"
#include "cec-log.h"
#include "cec-task.h"
#include "ddc.h"
#include "nvs.h"
#include "cec-validate.h"

/* Significantly rewritten from the initial Arduino version by Szymon Slupik:
 * https://github.com/SzymonSlupik/CEC-Tiny-Pro
 * which itself is based on the original code by Thomas Sowell:
 * https://github.com/tsowell/avr-hdmi-cec-volume/tree/master
 */

/** The running CEC configuration. */
static cec_config_t config = {0x0};

// HDMI logical addresses
// 2 dimensional array of valid logical addresses for playback and recording
// only.
#define NUM_LADDRESS 4
#define NUM_TYPES 6
static const uint8_t laddress[NUM_TYPES][NUM_LADDRESS] = {
    {0x00, 0x00, 0x00, 0x00},  // TV
    {0x01, 0x02, 0x09, 0x0f},  // Recording Device
    {0x0f, 0x0f, 0x0f, 0x0f},  // Reserved
    {0x03, 0x06, 0x07, 0x0f},  // Tuner + 0x0a
    {0x04, 0x08, 0x0b, 0x0f},  // Playback Device
    {0x05, 0x05, 0x05, 0x05},  // Audio System
};

/* The HDMI address for this device.  Respond to CEC sent to this address. */
static uint8_t laddr = 0x0f;

/* The HDMI physical address. */
static uint16_t paddr = 0x0000;

/* Active state. */
static uint16_t active_addr = 0x0000;

/* Audio state. */
static bool audio_status = false;

typedef struct {
  cec_message_t message;
  TaskHandle_t requester;
  uint32_t id;
  uint64_t deadline;
} cec_tx_request_t;

/* Outbound TX queue: holds one synchronous request at a time. */
#define CEC_TX_QUEUE_LEN 1
#define CEC_CLIENT_NOTIFY_TX ((UBaseType_t)0)
#define CEC_CLIENT_NOTIFY_POWER ((UBaseType_t)1)
static QueueHandle_t cec_tx_queue = NULL;
static StaticQueue_t cec_tx_queue_buf;
static uint8_t cec_tx_queue_storage[CEC_TX_QUEUE_LEN * sizeof(cec_tx_request_t)];
static TaskHandle_t power_status_waiter = NULL;
static uint32_t tx_generation;
static volatile uint32_t cancelled_tx;
static uint64_t power_query_since;
static uint64_t power_query_deadline;
static bool source_active;
static bool menu_state;
static uint64_t next_allocate;
static volatile uint32_t heartbeat;
uint32_t cec_heartbeat_ms(void) { return heartbeat; }
static uint16_t routing_address;
static uint64_t routing_at;

bool cec_ready(void) { return cec_tx_queue != NULL && laddr > 0 && laddr < 15; }
void cec_set_source_active(bool active) {
  taskENTER_CRITICAL(); source_active = active; if (active) active_addr = paddr; taskEXIT_CRITICAL();
}
bool cec_source_active(void) { return source_active; }
bool cec_routing_confirmed(uint16_t physical, uint64_t since) {
  taskENTER_CRITICAL();
  bool confirmed = routing_address == physical && routing_at >= since;
  taskEXIT_CRITICAL();
  return confirmed;
}
void cec_apply_config(const cec_config_t *value) {
  taskENTER_CRITICAL(); config = *value; laddr = 15; taskEXIT_CRITICAL();
}


static void cec_feature_abort(uint8_t initiator,
                              uint8_t destination,
                              uint8_t msg,
                              cec_abort_t reason) {
  cec_message_t message = {
      .header = HEADER0(initiator, destination),
      .opcode = CEC_ID_FEATURE_ABORT,
      .operand = {msg, reason},
      .len = 4,
  };

  cec_frame_send(&message);
}

static void device_vendor_id(uint8_t initiator, uint8_t destination, uint32_t vendor_id) {
  cec_message_t message = {
      .header = HEADER0(initiator, destination),
      .opcode = CEC_ID_DEVICE_VENDOR_ID,
      .operand = {(vendor_id >> 16) & 0x0ff, (vendor_id >> 8) & 0x0ff, (vendor_id >> 0) & 0x0ff},
      .len = 5,
  };

  cec_frame_send(&message);
}

static void report_power_status(uint8_t initiator, uint8_t destination, uint8_t power_status) {
  cec_message_t message = {.header = HEADER0(initiator, destination),
                           .opcode = CEC_ID_REPORT_POWER_STATUS,
                           .operand = {power_status},
                           .len = 3};

  cec_frame_send(&message);
}

static void set_system_audio_mode(uint8_t initiator,
                                  uint8_t destination,
                                  uint8_t system_audio_mode) {
  cec_message_t message = {
      .header = HEADER0(initiator, destination),
      .opcode = CEC_ID_SET_SYSTEM_AUDIO_MODE,
      .operand = {system_audio_mode},
      .len = 3,
  };

  cec_frame_send(&message);
}

static void report_audio_status(uint8_t initiator, uint8_t destination, uint8_t audio_status) {
  cec_message_t message = {.header = HEADER0(initiator, destination),
                           .opcode = CEC_ID_REPORT_AUDIO_STATUS,
                           .operand = {audio_status},
                           .len = 3};

  cec_frame_send(&message);
}

static void system_audio_mode_status(uint8_t initiator,
                                     uint8_t destination,
                                     uint8_t system_audio_mode_status) {
  cec_message_t message = {
      .header = HEADER0(initiator, destination),
      .opcode = CEC_ID_SYSTEM_AUDIO_MODE_STATUS,
      .operand = {system_audio_mode_status},
      .len = 3,
  };

  cec_frame_send(&message);
}

static void set_osd_name(uint8_t initiator, uint8_t destination) {
  uint8_t name_len = strnlen(config.osd_name, CEC_OSD_NAME_MAX_LEN);

  cec_message_t message = {
      .header = HEADER0(initiator, destination),
      .opcode = CEC_ID_SET_OSD_NAME,
      .len = 2 + name_len,
  };

  memcpy(message.operand, config.osd_name, name_len);

  cec_frame_send(&message);
}

static void report_physical_address(uint8_t initiator,
                                    uint8_t destination,
                                    uint16_t physical_address,
                                    uint8_t device_type) {
  cec_message_t message = {
      .header = HEADER0(initiator, destination),
      .opcode = CEC_ID_REPORT_PHYSICAL_ADDRESS,
      .operand = {(physical_address >> 8) & 0x0ff, (physical_address >> 0) & 0x0ff, device_type},
      .len = 5,
  };

  cec_frame_send(&message);
}

static void report_cec_version(uint8_t initiator, uint8_t destination) {
  cec_message_t message = {
      .header = HEADER0(initiator, destination),
      .opcode = CEC_ID_CEC_VERSION,
      .operand = {0x04},  // 0x04 = 1.3a
      .len = 3,
  };

  cec_frame_send(&message);
}

static cec_tx_result_t cec_ping(uint8_t destination) {
  cec_message_t message = {
      .header = HEADER0(destination, destination),
      .len = 1,
  };
  return cec_frame_send(&message);
}

cec_tx_result_t cec_send_msg_sync(const cec_message_t *msg, uint32_t timeout_ms) {
  if (cec_tx_queue == NULL || !timeout_ms || !cec_message_valid(msg)) return CEC_TX_UNAVAILABLE;
  taskENTER_CRITICAL();
  tx_generation = (tx_generation + 1) & 0xffffff;
  if (!tx_generation) tx_generation = 1;
  uint32_t id = tx_generation;
  taskEXIT_CRITICAL();
  cec_tx_request_t request = {
    .message = *msg, .requester = xTaskGetCurrentTaskHandle(), .id = id,
    .deadline = time_us_64() + (uint64_t)timeout_ms * 1000,
  };
  uint32_t result;
  xTaskNotifyWaitIndexed(CEC_CLIENT_NOTIFY_TX, 0, UINT32_MAX, &result, 0);
  if (xQueueSend(cec_tx_queue, &request, 0) != pdTRUE) return CEC_TX_UNAVAILABLE;
  xTaskNotifyIndexed(xCECTask, NOTIFY_RX, NOTIFY_RX_TX, eSetBits);
  while (true) {
    uint64_t now = time_us_64();
    if (now >= request.deadline) break;
    uint32_t remaining = (uint32_t)((request.deadline - now + 999) / 1000);
    if (xTaskNotifyWaitIndexed(CEC_CLIENT_NOTIFY_TX, 0, UINT32_MAX, &result,
                               pdMS_TO_TICKS(remaining)) == pdTRUE && (result >> 8) == id) {
      return (cec_tx_result_t)(result & 255);
    }
  }
  cancelled_tx = id;
  /* The wire deadline is the same deadline; allow the owning task to finish cleanup. */
  xTaskNotifyWaitIndexed(CEC_CLIENT_NOTIFY_TX, 0, UINT32_MAX, &result, pdMS_TO_TICKS(10));
  return CEC_TX_TIMEOUT;
}

static void active_source(uint8_t initiator, uint16_t physical_address) {
  if (!source_active || !cec_ready() || !cec_physical_address_valid(physical_address)) return;
  cec_message_t message = {
      .header = HEADER0(initiator, 0x0f),
      .opcode = CEC_ID_ACTIVE_SOURCE,
      .operand = {(physical_address >> 8) & 0x0ff, (physical_address >> 0) & 0x0ff},
      .len = 4,
  };

  cec_frame_send(&message);
}

static void menu_status(uint8_t initiator, uint8_t destination, bool menu_state) {
  uint8_t state = menu_state ? (uint8_t)CEC_MENU_ACTIVATE : (uint8_t)CEC_MENU_DEACTIVATE;
  cec_message_t message = {
      .header = HEADER0(initiator, destination),
      .opcode = CEC_ID_MENU_STATUS,
      .operand = {state},
      .len = 3,
  };

  cec_frame_send(&message);
}

static uint8_t allocate_logical_address(cec_config_t *config) {
  if (config->logical_address != 0x00 && config->logical_address != 0x0f) {
    return cec_ping(config->logical_address) == CEC_TX_NO_ACK ? config->logical_address : 15;
  }

  // Treat 0x00 or 0x0f as auto-allocate
  uint8_t a = 0x0f;
  if (config->device_type >= NUM_TYPES) return a;
  for (unsigned int i = 0; i < NUM_LADDRESS; i++) {
    a = laddress[config->device_type][i];
    if (a == 0x0f) break;
    cec_log_submitf("Attempting to allocate logical address 0x%01hhx"_LOG_BR, a);
    cec_tx_result_t result = cec_ping(a);
    if (result == CEC_TX_NO_ACK) {
      break;
    }
    if (result == CEC_TX_TIMEOUT) {
      a = 0x0f;
      break;
    }
  }

  cec_log_submitf("Allocated logical address 0x%02x"_LOG_BR, a);
  return a;
}

static uint16_t get_physical_address(const cec_config_t *config) {
  return (config->physical_address == 0x0000) ? ddc_get_physical_address()
                                              : config->physical_address;
}

uint16_t cec_get_physical_address(void) {
  return paddr;
}

uint8_t cec_get_logical_address(void) {
  return laddr;
}

void cec_set_physical_address(uint16_t physical_address) {
  taskENTER_CRITICAL();
  config.physical_address = physical_address;
  paddr = physical_address;
  taskEXIT_CRITICAL();
}

cec_power_query_result_t cec_query_power_status(uint8_t *power_status, uint32_t timeout_ms) {
  if (!power_status || !timeout_ms || !cec_ready()) return CEC_POWER_QUERY_UNAVAILABLE;
  TaskHandle_t current = xTaskGetCurrentTaskHandle();
  uint32_t result;
  xTaskNotifyWaitIndexed(CEC_CLIENT_NOTIFY_POWER, 0, UINT32_MAX, &result, 0);
  uint64_t deadline = time_us_64() + (uint64_t)timeout_ms * 1000;
  taskENTER_CRITICAL();
  if (power_status_waiter != NULL) { taskEXIT_CRITICAL(); return CEC_POWER_QUERY_UNAVAILABLE; }
  power_status_waiter = current;
  power_query_since = time_us_64(); power_query_deadline = deadline;
  taskEXIT_CRITICAL();
  cec_message_t query = { .header = HEADER0(laddr, 0), .opcode = CEC_ID_GIVE_DEVICE_POWER_STATUS, .len = 2 };
  cec_tx_result_t tx_result = cec_send_msg_sync(&query, timeout_ms < 1500 ? timeout_ms : 1500);
  uint64_t now = time_us_64();
  if (tx_result == CEC_TX_ACK && now < deadline) {
    uint32_t remaining = (uint32_t)((deadline - now + 999) / 1000);
    if (xTaskNotifyWaitIndexed(CEC_CLIENT_NOTIFY_POWER, 0, UINT32_MAX, &result,
                               pdMS_TO_TICKS(remaining)) != pdTRUE) result = 0;
  } else result = 0;
  taskENTER_CRITICAL();
  if (power_status_waiter == current) power_status_waiter = NULL;
  taskEXIT_CRITICAL();
  if (result >= 0x100) return CEC_POWER_QUERY_UNSUPPORTED;
  if (result >= 1 && result <= 4) { *power_status = (uint8_t)(result - 1); return CEC_POWER_QUERY_OK; }
  if (tx_result == CEC_TX_NO_ACK) return CEC_POWER_QUERY_NO_ACK;
  if (tx_result != CEC_TX_ACK) return tx_result == CEC_TX_UNAVAILABLE ? CEC_POWER_QUERY_UNAVAILABLE : CEC_POWER_QUERY_TX_TIMEOUT;
  return CEC_POWER_QUERY_RESPONSE_TIMEOUT;
}

static void dispatch_message(cec_message_t msg) {
  if (msg.len > 1 && cec_message_valid(&msg) && cec_ready()) {
    uint64_t arrived = cec_frame_received_at();
    uint8_t initiator = (msg.header & 0xf0) >> 4;
    uint8_t destination = msg.header & 0x0f;
    if (initiator == laddr) {
      /* Our own TX is not received: another initiator is using our address. */
      laddr = 15; source_active = false; next_allocate = time_us_64() + 2000000;
      return;
    }

    switch (msg.opcode) {
      case CEC_ID_IMAGE_VIEW_ON:
        break;
      case CEC_ID_TEXT_VIEW_ON:
        break;
      case CEC_ID_STANDBY:
        if (destination == laddr || destination == 0x0f) {
          active_addr = 0x0000;
          source_active = false;
          blink_set_blink(BLINK_STATE_BLUE_2HZ);
        }
        break;
      case CEC_ID_SYSTEM_AUDIO_MODE_REQUEST:
        if (destination == laddr) {
          set_system_audio_mode(laddr, initiator, audio_status);
        }
        break;
      case CEC_ID_GIVE_AUDIO_STATUS:
        if (destination == laddr) {
          report_audio_status(laddr, initiator, 0x32);  // volume 50%, mute off
        }
        break;
      case CEC_ID_SET_SYSTEM_AUDIO_MODE:
        if (destination == laddr || destination == 0x0f) {
          audio_status = (msg.operand[0] == 1);
        }
        break;
      case CEC_ID_GIVE_SYSTEM_AUDIO_MODE_STATUS:
        if (destination == laddr)
          system_audio_mode_status(laddr, initiator, audio_status);
        break;
      case CEC_ID_SYSTEM_AUDIO_MODE_STATUS:
        break;
      case CEC_ID_ROUTING_CHANGE:
        active_addr = (msg.operand[2] << 8) | msg.operand[3];
        if (initiator == 0) { routing_address = active_addr; routing_at = arrived; }
        if (active_addr != paddr) source_active = false;
        paddr = get_physical_address(&config);
        if (!cec_ready()) laddr = allocate_logical_address(&config);
        if (paddr == active_addr) {
          active_source(laddr, paddr);
        }
        break;
      case CEC_ID_ROUTING_INFORMATION:
        if (initiator == 0) {
          routing_address = (msg.operand[0] << 8) | msg.operand[1]; routing_at = arrived;
          if (routing_address != paddr) source_active = false;
        }
        break;
      case CEC_ID_ACTIVE_SOURCE:
        if (initiator != laddr) { source_active = false; routing_address = 0xffff; routing_at = 0; }
        active_addr = (msg.operand[0] << 8) | msg.operand[1];
        break;
      case CEC_ID_REPORT_PHYSICAL_ADDRESS:
        // On broadcast receive, do the same
        if ((initiator == 0x00) && (destination == 0x0f)) {
          paddr = get_physical_address(&config);
          if (!cec_ready()) laddr = allocate_logical_address(&config);
          if (paddr != 0x0000) {
            report_physical_address(laddr, 0x0f, paddr, config.device_type);
          }
        }
        break;
      case CEC_ID_REQUEST_ACTIVE_SOURCE:
        if (source_active && paddr == active_addr) {
          active_source(laddr, paddr);
        }
        break;
      case CEC_ID_SET_STREAM_PATH:
        if (source_active && paddr == ((msg.operand[0] << 8) | msg.operand[1])) {
          active_addr = paddr;
          active_source(laddr, paddr);
          menu_state = true;
          menu_status(laddr, 0x00, menu_state);

          blink_set_blink(BLINK_STATE_GREEN_2HZ);
        }
        break;
      case CEC_ID_DEVICE_VENDOR_ID:
        // On broadcast receive, do the same
        if ((initiator == 0x00) && (destination == 0x0f)) {
          device_vendor_id(laddr, 0x0f, 0x0010FA);
        }
        break;
      case CEC_ID_GIVE_DEVICE_VENDOR_ID:
        if (destination == laddr)
          device_vendor_id(laddr, 0x0f, 0x0010FA);
        break;
      case CEC_ID_MENU_STATUS:
        break;
      case CEC_ID_MENU_REQUEST:
        if (destination == laddr) {
          cec_menu_t request = msg.operand[0];
          switch (request) {
            case CEC_MENU_ACTIVATE:
              menu_state = true;
              break;
            case CEC_MENU_DEACTIVATE:
              menu_state = false;
              break;
            case CEC_MENU_QUERY:
              break;
          }
          menu_status(laddr, initiator, menu_state);
        }
        break;
      case CEC_ID_GIVE_DEVICE_POWER_STATUS:
        if (destination == laddr)
          report_power_status(laddr, initiator, active_addr != paddr);
  #if 0
        /* Hack for Google Chromecast to force it sending V+/V- if no CEC TV is present */
        if (destination == 0)
          report_power_status(0, initiator, 0x00);
  #endif
        break;
      case CEC_ID_REPORT_POWER_STATUS:
        if (initiator == 0x00 && (destination == laddr || destination == 15)
            && msg.operand[0] <= 3 && arrived >= power_query_since && arrived <= power_query_deadline) {
          TaskHandle_t waiter = NULL;
          taskENTER_CRITICAL();
          waiter = power_status_waiter;
          power_status_waiter = NULL;
          taskEXIT_CRITICAL();
          if (waiter != NULL) {
            xTaskNotifyIndexed(waiter,
                               CEC_CLIENT_NOTIFY_POWER,
                               (uint32_t)msg.operand[0] + 1,
                               eSetValueWithOverwrite);
          }
        }
        break;
      case CEC_ID_GET_MENU_LANGUAGE:
        break;
      case CEC_ID_INACTIVE_SOURCE:
        break;
      case CEC_ID_CEC_VERSION:
        break;
      case CEC_ID_GET_CEC_VERSION:
        if (destination == laddr) {
          report_cec_version(laddr, initiator);
        }
        break;
      case CEC_ID_GIVE_OSD_NAME:
        if (destination == laddr)
          set_osd_name(laddr, initiator);
        break;
      case CEC_ID_SET_OSD_NAME:
        break;
      case CEC_ID_GIVE_PHYSICAL_ADDRESS:
        if (destination == laddr && paddr != 0x0000)
          report_physical_address(laddr, 0x0f, paddr, config.device_type);
        break;
      case CEC_ID_USER_CONTROL_PRESSED:
        if (destination == laddr) {
          blink_set(BLINK_STATE_GREEN_ON);
        }
        break;
      case CEC_ID_USER_CONTROL_RELEASED:
        if (destination == laddr) {
          blink_set(BLINK_STATE_OFF);
        }
        break;
      case CEC_ID_ABORT:
        if (destination == laddr) {
          cec_feature_abort(laddr, initiator, msg.opcode, CEC_ABORT_REFUSED);
        }
        break;
      case CEC_ID_FEATURE_ABORT:
        if (initiator == 0 && destination == laddr && msg.operand[0] == CEC_ID_GIVE_DEVICE_POWER_STATUS
            && arrived >= power_query_since && arrived <= power_query_deadline) {
          taskENTER_CRITICAL();
          TaskHandle_t waiter = power_status_waiter; power_status_waiter = NULL;
          taskEXIT_CRITICAL();
          if (waiter) xTaskNotifyIndexed(waiter, CEC_CLIENT_NOTIFY_POWER, 0x100u + msg.operand[1], eSetValueWithOverwrite);
        }
        break;
      case CEC_ID_VENDOR_COMMAND_WITH_ID:
        break;
      default:
        if (destination == laddr) {
          cec_feature_abort(laddr, initiator, msg.opcode, CEC_ABORT_UNRECOGNIZED);
        }
        break;
    }
  }
}

static bool service_tx_request(void) {
  cec_tx_request_t request;
  if (xQueueReceive(cec_tx_queue, &request, 0) == pdTRUE) {
    cec_tx_result_t result = CEC_TX_TIMEOUT;
    uint64_t now = time_us_64();
    if (request.id != cancelled_tx && now < request.deadline
        && (request.message.header >> 4) == laddr && cec_ready()) {
      uint32_t remaining = (uint32_t)((request.deadline - now) / 1000);
      bool query = request.message.opcode == CEC_ID_GIVE_DEVICE_POWER_STATUS && request.message.len > 1;
      if (query) power_query_since = UINT64_MAX;
      result = cec_frame_send_timeout(&request.message, remaining);
      if (query && result == CEC_TX_ACK) power_query_since = cec_frame_sent_at();
    }
    xTaskNotifyIndexed(request.requester, CEC_CLIENT_NOTIFY_TX,
                       (request.id << 8) | (uint32_t)result, eSetValueWithOverwrite);
    return true;
  }
  return false;
}

void cec_task(void *param) {
  (void)param;

  menu_state = false;

  // load configuration
  nvs_load_config(&config);

  // pause for EDID to settle
#if CEC_ENABLE_DDC
  for (uint32_t elapsed = 0; elapsed < config.edid_delay_ms; elapsed += 100) {
    heartbeat = (uint32_t)(time_us_64() / 1000);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
#endif

  cec_frame_init();

  paddr = get_physical_address(&config);
  laddr = allocate_logical_address(&config);
  cec_tx_queue = xQueueCreateStatic(CEC_TX_QUEUE_LEN, sizeof(cec_tx_request_t),
                                    cec_tx_queue_storage, &cec_tx_queue_buf);
  next_allocate = time_us_64() + 2000000;

  while (true) {
    heartbeat = (uint32_t)(time_us_64() / 1000);
    cec_message_t msg = {0x0};

    cec_frame_recv(&msg, laddr);

    dispatch_message(msg);
    if (!service_tx_request() && !cec_ready() && time_us_64() >= next_allocate && cec_frame_idle()) {
      laddr = allocate_logical_address(&config);
      next_allocate = time_us_64() + 2000000;
    }
  }
}
