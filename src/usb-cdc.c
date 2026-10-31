#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <tusb.h>

#include <hardware/gpio.h>
#include <hardware/watchdog.h>
#include <pico/bootrom.h>
#include <pico/rand.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "cec-validate.h"
#include "cec-id.h"

#include "pico-cec/config.h"
#include "pico-cec/util.h"

#include "cec-frame.h"
#include "cec-control.h"
#include "cec-log.h"
#include "cec-task.h"
#include "nvs.h"
#include "tclie.h"
#include "usb-cdc.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

// Copy of configuration
static cec_config_t config = {0x0};

static tclie_t tclie;

#define OUTPUT_LENGTH 384
#define OUTPUT_COUNT 8
#define RESULT_COUNT 8
static TaskHandle_t parser_task;
static QueueHandle_t output_queue, operation_queue;
static StaticQueue_t output_queue_static, operation_queue_static;
typedef struct { char text[OUTPUT_LENGTH]; } output_t;
static uint8_t output_storage[OUTPUT_COUNT * sizeof(output_t)];
static uint64_t session_id;
static uint64_t manual_id;
static volatile bool operation_busy;
static volatile uint32_t heartbeat;
uint32_t cdc_heartbeat_ms(void) { return heartbeat; }
static const char *operation_phase = "idle";
typedef struct { uint64_t id; char command[8]; uint16_t physical; } operation_t;
static operation_t current_operation;
static uint8_t operation_storage[sizeof(operation_t)];
typedef struct { operation_t request; char text[OUTPUT_LENGTH]; } retained_t;
static retained_t retained[RESULT_COUNT];
static unsigned retained_next;
typedef struct { bool activation_started; uint64_t operation_at; } control_context_t;

static bool write_usb(const char *str) {
  size_t length = strlen(str), used = 0;
  uint64_t deadline = util_uptime_ms() + 200;
  while (used < length && tud_cdc_connected() && util_uptime_ms() < deadline) {
    used += tud_cdc_write(str + used, (uint32_t)(length - used));
    tud_cdc_write_flush();
    if (used < length) vTaskDelay(pdMS_TO_TICKS(1));
  }
  return used == length;
}
static void print(const char *str) {
  if (xTaskGetCurrentTaskHandle() == parser_task) { (void)write_usb(str); return; }
  output_t item;
  if (snprintf(item.text, sizeof(item.text), "%s", str) >= (int)sizeof(item.text)) return;
  if (output_queue) xQueueSend(output_queue, &item, pdMS_TO_TICKS(100));
}
static void tcli_print(void *arg, const char *str) { (void)arg; print(str); }
static void cdc_vprintf(const char *fmt, va_list ap, bool newline) {
  char buffer[OUTPUT_LENGTH];
  int length = vsnprintf(buffer, sizeof(buffer) - 2, fmt, ap);
  if (length < 0 || length >= (int)sizeof(buffer) - 2) return;
  if (newline) { buffer[length++] = '\r'; buffer[length++] = '\n'; buffer[length] = 0; }
  print(buffer);
}
void cdc_log(const char *str) {
  if (!output_queue || uxQueueSpacesAvailable(output_queue) < 2) return;
  output_t item;
  snprintf(item.text, sizeof(item.text), "%s", str);
  xQueueSend(output_queue, &item, 0);
}
void cdc_printf(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); cdc_vprintf(fmt, ap, false); va_end(ap);
}
void cdc_printfln(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); cdc_vprintf(fmt, ap, true); va_end(ap);
}

static cec_tx_result_t control_send(void *context, const cec_message_t *message, uint32_t timeout) {
  control_context_t *state = context;
  if (!tud_cdc_connected()) return CEC_TX_UNAVAILABLE;
  operation_phase = message->opcode == CEC_ID_ACTIVE_SOURCE || message->opcode == CEC_ID_REPORT_PHYSICAL_ADDRESS
                  ? "activate" : "power";
  if (message->opcode == CEC_ID_ACTIVE_SOURCE) {
    if (!state->activation_started) {
      state->activation_started = true;
      cec_set_source_active(true);
    } else if (!cec_source_active()) return CEC_TX_UNAVAILABLE;
  }
  return cec_send_msg_sync(message, timeout);
}
static void control_delay(void *context, uint32_t duration) {
  (void)context; if (duration) vTaskDelay(pdMS_TO_TICKS(duration));
}
static uint64_t control_now(void *context) { (void)context; return util_uptime_ms(); }
static cec_power_query_result_t control_query(void *context, uint8_t initiator,
                                              uint8_t *status, uint32_t timeout) {
  (void)context;
  if (!tud_cdc_connected() || initiator != cec_get_logical_address()) return CEC_POWER_QUERY_UNAVAILABLE;
  return cec_query_power_status(status, timeout);
}
static bool control_route(void *context, uint16_t physical) {
  control_context_t *state = context;
  return cec_source_active() && cec_routing_confirmed(physical, state->operation_at);
}
static void operation_worker(void *context) {
  (void)context;
  operation_t request;
  while (true) {
    xQueueReceive(operation_queue, &request, portMAX_DELAY);
    operation_phase = "ready";
    uint64_t started_us = time_us_64();
    uint64_t started = started_us / 1000;
    uint32_t limit = strcmp(request.command, "on") == 0 ? CEC_CONTROL_ON_TIMEOUT_MS
                   : strcmp(request.command, "standby") == 0 ? CEC_CONTROL_STANDBY_TIMEOUT_MS
                   : CEC_CONTROL_STATUS_TIMEOUT_MS;
    uint64_t deadline = started + limit;
    while (!cec_ready() && util_uptime_ms() + 50 < deadline && tud_cdc_connected())
      vTaskDelay(pdMS_TO_TICKS(50));
    control_context_t state = { .operation_at = started_us };
    cec_control_bus_t bus = { .context = &state, .send = control_send,
      .delay = control_delay, .query_power = control_query, .now_ms = control_now,
      .routing_confirmed = control_route, .deadline_ms = deadline };
    cec_control_result_t result = { .code = CEC_CONTROL_UNAVAILABLE,
      .power_status = 255, .phase = "ready" };
    if (cec_ready() && tud_cdc_connected() && util_uptime_ms() < deadline) {
      if (strcmp(request.command, "on") == 0) {
        config.physical_address = request.physical;
        cec_set_physical_address(request.physical);
        result = cec_control_on(&bus, cec_get_logical_address(), request.physical, config.device_type);
      } else if (strcmp(request.command, "standby") == 0) {
        cec_set_source_active(false);
        result = cec_control_standby(&bus, cec_get_logical_address());
      } else { operation_phase = "status"; result = cec_control_status(&bus, cec_get_logical_address()); }
    }
    result.elapsed_ms = (uint32_t)(util_uptime_ms() - started);
    retained_t entry = { .request = request };
    snprintf(entry.text, sizeof(entry.text),
      "\r\nCECCTRL/2 %s command=%s id=%016llx session=%016llx code=%s power=%s value=%u verified=%u source=%s routing=%s attempts=%u elapsed_ms=%lu phase=%s\r\n",
      result.code == CEC_CONTROL_OK ? "OK" : "ERR", request.command,
      (unsigned long long)request.id, (unsigned long long)session_id,
      cec_control_result_name(result.code), cec_control_power_name(result.power_status),
      result.power_status, result.power_confirmed, result.source_announced ? "announced" : "none",
      result.routing_confirmed ? "confirmed" : "unconfirmed", result.attempts,
      (unsigned long)result.elapsed_ms, result.phase);
    taskENTER_CRITICAL(); retained[retained_next] = entry;
    retained_next = (retained_next + 1) % RESULT_COUNT; taskEXIT_CRITICAL();
    print(entry.text);
    operation_busy = false; operation_phase = "idle";
  }
}

static int show_version(void *arg) {
  cdc_printfln("%s", PICO_CEC_VERSION);
  return 0;
}

static int exec_debug(void *arg, int argc, const char *argv[]) {
  if (argc == 2) {
    if (strcmp(argv[1], "on") == 0) {
      cec_log_enable();
      return 0;
    } else if (strcmp(argv[1], "off") == 0) {
      cec_log_disable();
      return 0;
    }
  }

  return -1;
}

static int exec_reboot(void *arg, int argc, const char **argv) {
  if (operation_busy) { cdc_printfln("Controller busy; wait for the active operation."); return -1; }
  if ((argc == 2) && (strcmp(argv[1], "bootsel") == 0)) {
    // reboot into USB bootloader
#ifdef PICO_DEFAULT_LED_PIN
    uint32_t activity_mask = 1u << PICO_DEFAULT_LED_PIN;
#else
    uint32_t activity_mask = 0;
#endif
    watchdog_disable();
    reset_usb_boot(activity_mask, 0);
  } else {
    // normal reboot
    watchdog_reboot(0, 0, 0);
  }

  return -1;
}

static void print_edid_delay(uint32_t delay) {
  cdc_printfln("%-17s: %lu ms", "EDID delay", delay);
}

static void print_physical_address(uint16_t address) {
  cdc_printfln("%-17s: 0x%04x", "Physical address", address);
}

static void print_logical_address(uint8_t address) {
  cdc_printfln("%-17s: 0x%02x", "Logical address", address);
}

static void print_osd_name(const char *name) {
  cdc_printfln("%-17s: %s", "OSD name", name);
}

static int show_config(cec_config_t *config) {
  // UBaseType_t uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
  // cdc_printfln("StackHighWaterMark = %lu", uxHighWaterMark);

  print_edid_delay(config->edid_delay_ms);
  print_physical_address(config->physical_address);
  print_logical_address(config->logical_address);
  const char *type = "unknown";
  switch ((cec_config_device_type_t)config->device_type) {
    case CEC_CONFIG_DEVICE_TYPE_TV:
      type = "TV";
      break;
    case CEC_CONFIG_DEVICE_TYPE_RECORDING:
      type = "recording";
      break;
    case CEC_CONFIG_DEVICE_TYPE_RESERVED:
      type = "reserved";
      break;
    case CEC_CONFIG_DEVICE_TYPE_PLAYBACK:
      type = "playback";
      break;
    case CEC_CONFIG_DEVICE_TYPE_TUNER:
      type = "tuner";
      break;
    case CEC_CONFIG_DEVICE_TYPE_AUDIO_SYSTEM:
      type = "audio";
      break;
  }
  cdc_printfln("%-17s: %s", "Device type", type);

  print_osd_name(config->osd_name);

  return 0;
}

static int show_stats_cec(void) {
  cec_frame_stats_t stats = {0x0};
  cec_frame_get_stats(&stats);
  cdc_printfln("%-13s: %lu frames", "CEC rx", stats.rx_frames);
  cdc_printfln("%-13s: %lu frames", "CEC tx", stats.tx_frames);
  cdc_printfln("%-13s: %lu frames", "CEC rx abort", stats.rx_abort_frames);
  cdc_printfln("%-13s: %lu frames", "CEC tx noack", stats.tx_noack_frames);
  cdc_printfln("%-13s: %lu frames", "CEC tx timeout", stats.tx_timeout_frames);
  cdc_printfln("%-13s: %lu frames", "CEC tx idle", stats.tx_idle_timeout_frames);
  cdc_printfln("%-13s: %lu frames", "CEC tx alarm", stats.tx_alarm_timeout_frames);
  cdc_printfln("%-13s: %lu frames", "Arbitration", stats.tx_arbitration_frames);
  cdc_printfln("%-13s: %lu frames", "Bus error", stats.tx_bus_error_frames);

  return 0;
}

static int show_stats_cpu(void) {
  UBaseType_t count = uxTaskGetNumberOfTasks();
  TaskStatus_t status[count];
  unsigned long total_run_time = 0;

  UBaseType_t n = uxTaskGetSystemState(status, count, &total_run_time);

  uint64_t uptime = util_uptime_ms() / 1000;
  uint64_t seconds = uptime % 60;
  uptime /= 60;
  uint64_t minutes = uptime % 60;
  uptime /= 60;
  uint64_t hours = uptime % 24;
  uint64_t days = uptime / 24;

  cdc_printfln("%-13s: %llud %lluh %llum %llus", "Uptime", days, hours, minutes, seconds);

  for (UBaseType_t i = 0; i < n; i++) {
    cdc_printfln("%-13s: %7.3f %%", status[i].pcTaskName,
                 (100.0f * status[i].ulRunTimeCounter) / total_run_time);
  }

  return 0;
}

static int show_stats_tasks(void) {
  UBaseType_t count = uxTaskGetNumberOfTasks();
  TaskStatus_t status[count];

  UBaseType_t n = uxTaskGetSystemState(status, count, NULL);

  cdc_printfln("%-13s | %-10s | %-10s", "task", "priority", "stack (min)");

  for (UBaseType_t i = 0; i < n; i++) {
    cdc_printfln("%-13s | %-10lu | %-10lu", status[i].pcTaskName,
                 (uint32_t)status[i].uxCurrentPriority, (uint32_t)status[i].usStackHighWaterMark);
  }

  return 0;
}

static int exec_show(void *arg, int argc, const char **argv) {
  if (argc == 2) {
    if (strcmp(argv[1], "config") == 0) {
      return show_config(&config);
    } else if (strcmp(argv[1], "cec") == 0) {
      print_physical_address(cec_get_physical_address());
      print_logical_address(cec_get_logical_address());
      cdc_printfln("%-17s: GP%u", "CEC GPIO", CEC_PIN);
      cdc_printfln("%-17s: %s", "CEC direction",
                   gpio_get_dir(CEC_PIN) == GPIO_OUT ? "output" : "input");
      cdc_printfln("%-17s: %s", "CEC line", gpio_get(CEC_PIN) ? "high" : "low");
    } else if (strcmp(argv[1], "version") == 0) {
      return show_version(arg);
    } else if (strcmp(argv[1], "nvs") == 0) {
      cec_config_t nvs_config;
      if (nvs_read_config(&nvs_config)) {
        return show_config(&nvs_config);
      } else {
        cdc_printfln("Failed to read configuration from NVS.");
        return -1;
      }
    }
  } else if (argc == 3) {
    if (strcmp(argv[1], "stats") == 0) {
      if (strcmp(argv[2], "cec") == 0) {
        return show_stats_cec();
      } else if (strcmp(argv[2], "cpu") == 0) {
        return show_stats_cpu();
      } else if (strcmp(argv[2], "tasks") == 0) {
        return show_stats_tasks();
      }
    }
  }

  return -1;
}

static int exec_save(void *arg, int argc, const char **argv) {
  if (operation_busy) { cdc_printfln("Controller busy; wait for the active operation."); return -1; }
  // UBaseType_t uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
  // cdc_printfln("StackHighWaterMark = %lu", uxHighWaterMark);

  if (!cec_frame_idle()) { cdc_printfln("CEC bus busy; retry save when idle."); return -1; }
  bool r = nvs_save_config(&config);

  // cdc_printfln("r = %u", r);

  return r ? 0 : -1;
}

static int exec_set(void *arg, int argc, const char **argv) {
  (void)arg;
  if (operation_busy) { cdc_printfln("Controller busy; wait for the active operation."); return -1; }
  if (argc != 4 || strcmp(argv[1], "config")) return -1;
  cec_config_t candidate = config;
  const char *key = argv[2], *value = argv[3];
  if (!strcmp(key, "osd_name")) {
    if (strlen(value) > CEC_OSD_NAME_MAX_LEN) return -1;
    snprintf(candidate.osd_name, sizeof(candidate.osd_name), "%s", value);
  } else if (!strcmp(key, "device_type")) {
    if (!strcmp(value, "playback")) candidate.device_type = CEC_CONFIG_DEVICE_TYPE_PLAYBACK;
    else if (!strcmp(value, "recording")) candidate.device_type = CEC_CONFIG_DEVICE_TYPE_RECORDING;
    else return -1;
    candidate.logical_address = 15;
  } else {
    if (!*value || *value == '-' || *value == '+') return -1;
    bool decimal = !strcmp(key, "edid_delay_ms");
    for (const char *p = value; *p; p++)
      if (!(decimal ? isdigit((unsigned char)*p) : isxdigit((unsigned char)*p))) return -1;
    char *end; unsigned long n = strtoul(value, &end, decimal ? 10 : 16);
    if (*end) return -1;
    if (decimal) { if (n > 30000) return -1; candidate.edid_delay_ms = (uint32_t)n; }
    else if (!strcmp(key, "physical_address")) {
      if (strlen(value) != 4 || !cec_physical_address_valid((uint16_t)n)) return -1;
      candidate.physical_address = (uint16_t)n;
    } else if (!strcmp(key, "logical_address")) {
      if (strlen(value) > 2 || n > 15 || n == 0) return -1;
      candidate.logical_address = (uint8_t)n;
    } else return -1;
  }
  if (!cec_config_valid(&candidate)) return -1;
  config = candidate;
  cec_apply_config(&config);
  return show_config(&config);
}

static const char *tx_result_name(cec_tx_result_t result) {
  switch (result) {
    case CEC_TX_ACK:
      return "ack";
    case CEC_TX_NO_ACK:
      return "no_ack";
    case CEC_TX_TIMEOUT:
      return "timeout";
    case CEC_TX_ARBITRATION_LOST:
      return "arbitration_lost";
    case CEC_TX_BUS_ERROR:
      return "bus_error";
    case CEC_TX_UNAVAILABLE:
    default:
      return "unavailable";
  }
}

static void operation_error(const char *command, uint64_t id, const char *code) {
  cdc_printfln("CECCTRL/2 ERR command=%s id=%016llx session=%016llx code=%s",
               command, (unsigned long long)id, (unsigned long long)session_id, code);
}
static bool parse_id(const char *text, uint64_t *id) {
  if (!*text || strlen(text) > 16) return false;
  for (const char *p = text; *p; p++) if (!isxdigit((unsigned char)*p)) return false;
  char *end; *id = strtoull(text, &end, 16);
  return !*end && *id != 0;
}
static int exec_tv(void *arg, int argc, const char **argv) {
  (void)arg;
  if (argc < 2 || argc > 4) return -1;
  const char *command = argv[1];
  if (strcmp(command, "protocol") == 0 && argc == 2) {
    cdc_printfln("CECCTRL/2 OK command=protocol firmware=%s session=%016llx on_ms=%u standby_ms=%u status_ms=%u gpio=%u ready=%u busy=%u active=%016llx phase=%s",
      PICO_CEC_VERSION, (unsigned long long)session_id, CEC_CONTROL_ON_TIMEOUT_MS,
      CEC_CONTROL_STANDBY_TIMEOUT_MS, CEC_CONTROL_STATUS_TIMEOUT_MS, CEC_PIN, cec_ready(), operation_busy,
      (unsigned long long)(operation_busy ? current_operation.id : 0), operation_phase);
    return 0;
  }
  operation_t request = { .id = ++manual_id, .physical = cec_get_physical_address() };
  bool on = strcmp(command, "on") == 0;
  bool result_query = strcmp(command, "result") == 0;
  if (!on && !result_query && strcmp(command, "standby") != 0 && strcmp(command, "status") != 0) return -1;
  if ((!on && argc > 3) || (result_query && argc != 3)) return -1;
  if (on && argc >= 3) {
    unsigned physical; char extra;
    if (strlen(argv[2]) != 4 || sscanf(argv[2], "%4x%c", &physical, &extra) != 1
        || !cec_physical_address_valid((uint16_t)physical)) {
      operation_error(command, request.id, "invalid_physical_address"); return -1;
    }
    request.physical = (uint16_t)physical;
  }
  int id_index = on ? 3 : 2;
  if (argc > id_index && !parse_id(argv[id_index], &request.id)) return -1;
  snprintf(request.command, sizeof(request.command), "%s", command);
  retained_t cached = {0};
  taskENTER_CRITICAL();
  for (unsigned i = 0; i < RESULT_COUNT; i++) {
    if (retained[i].request.id == request.id) { cached = retained[i]; break; }
  }
  taskEXIT_CRITICAL();
  if (cached.request.id) {
    if (!result_query && (strcmp(cached.request.command, command) != 0
                         || (on && cached.request.physical != request.physical))) {
      operation_error(command, request.id, "id_conflict"); return -1;
    }
    print(cached.text); return 0;
  }
  if (operation_busy) { operation_error(command, request.id, "busy"); return -1; }
  if (result_query) { operation_error(command, request.id, "unknown_request"); return -1; }
  if (on && !cec_physical_address_valid(request.physical)) {
    operation_error(command, request.id, "invalid_physical_address"); return -1;
  }
  operation_busy = true; current_operation = request;
  if (xQueueSend(operation_queue, &request, 0) != pdTRUE) {
    operation_busy = false; operation_error(command, request.id, "busy"); return -1;
  }
  return 0;
}

static int exec_send(void *arg, int argc, const char **argv) {
  if (operation_busy) { cdc_printfln("Controller busy; wait for the active operation."); return -1; }
  (void)arg;
  if (argc >= 3) {
    unsigned int address = 0;
    unsigned int opcode = 0;
    if (sscanf(argv[1], "%x", &address) == 1 && address <= 0x0f
        && sscanf(argv[2], "%x", &opcode) == 1 && opcode <= UINT8_MAX) {
      cec_message_t msg = {0};
      msg.header = HEADER0(cec_get_logical_address(), (uint8_t)address);
      msg.opcode = (uint8_t)opcode;
      msg.len = 2;  // header + opcode

      // Parse optional operands (hex bytes)
      for (int i = 3; i < argc && msg.len < CEC_FRAME_MAX_LEN; i++) {
        unsigned int byte;
        if (sscanf(argv[i], "%x", &byte) == 1 && byte <= 0xff) {
          msg.operand[msg.len - 2] = (uint8_t)byte;
          msg.len++;
        } else {
          cdc_printfln("Error parsing operand %d", i);
          return -1;
        }
      }

      cec_tx_result_t result = cec_send_msg_sync(&msg, CEC_CONTROL_TX_TIMEOUT_MS);
      bool success = address == 0x0f ? result == CEC_TX_ACK || result == CEC_TX_NO_ACK
                                     : result == CEC_TX_ACK;
      if (success) {
        cdc_printfln("CECCTRL/%u OK command=send result=%s",
                     CEC_CONTROL_PROTOCOL_VERSION,
                     address == 0x0f ? "complete" : tx_result_name(result));
        return 0;
      }
      cdc_printfln("CECCTRL/%u ERR command=send code=%s",
                   CEC_CONTROL_PROTOCOL_VERSION,
                   tx_result_name(result));
      return -1;
    }
    cdc_printfln("Error parsing opcode");
  }
  return -1;
}

static const tclie_cmd_t cmds[] = {
    {"debug", exec_debug, "Control debug output.", "debug {on|off}"},
    {"save", exec_save, "Save configuration.", "save"},
    {"send", exec_send, "Send a CEC opcode.",
     "send <addr> <opcode> [<operand>] [<operand>] [<operand>]"},
    {"set", exec_set, "Set configuration parameters.",
     "set {(config (edid_delay_ms|logical_address|physical_address|osd_name <value>)|(device_type "
     "{playback|recording}))}"},
    {"show", exec_show, "Show information.",
     "show {cec|config|nvs|(stats {cec|cpu|tasks})|version}"},
    {"tv", exec_tv, "Control or query the TV.",
     CEC_TV_COMMAND_PATTERN},
    {"reboot", exec_reboot, "Reboot system.", "reboot [bootsel]"},
};

void cdc_task(void *params) {
  (void)params;

  parser_task = xTaskGetCurrentTaskHandle();
  session_id = get_rand_64(); if (!session_id) session_id = 1;
  manual_id = session_id;
  output_queue = xQueueCreateStatic(OUTPUT_COUNT, sizeof(output_t), output_storage, &output_queue_static);
  operation_queue = xQueueCreateStatic(1, sizeof(operation_t), operation_storage, &operation_queue_static);
  static StackType_t worker_stack[1536]; static StaticTask_t worker_tcb;
  xTaskCreateStatic(operation_worker, "control", 1536, NULL, 1, worker_stack, &worker_tcb);
  nvs_load_config(&config);

  tclie_init(&tclie, tcli_print, NULL);
  tclie_reg_cmds(&tclie, cmds, ARRAY_SIZE(cmds));

  while (1) {
    heartbeat = (uint32_t)util_uptime_ms();
    output_t output;
    while (xQueueReceive(output_queue, &output, 0) == pdTRUE) {
      if (tud_cdc_connected()) (void)write_usb(output.text);
    }
    // connected() check for DTR bit
    // Most but not all terminal client set this when making connection
    if (tud_cdc_connected()) {
      // There are data available
      while (tud_cdc_available()) {
        uint8_t c = tud_cdc_read_char();
        tclie_in_char(&tclie, c);
        if (xQueueReceive(output_queue, &output, 0) == pdTRUE)
          (void)write_usb(output.text);
      }

      tud_cdc_write_flush();
      vTaskDelay(pdMS_TO_TICKS(1));
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
  (void)itf;
  (void)rts;

  if (dtr) {
    // Terminal connected
    print("Connected" _CDC_BR);
  } else {
    // Terminal disconnected
    print("Disconnected" _CDC_BR);
  }
}
