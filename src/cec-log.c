#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "message_buffer.h"
#include "task.h"

#include "pico-cec/config.h"
#include "pico-cec/util.h"

#include "cec-frame.h"
#include "cec-id.h"
#include "cec-log.h"
#include "cec-validate.h"

#define LOG_LINE_LENGTH (128)
#define LOG_QUEUE_LENGTH (16)
#define LOG_MB_SIZE (LOG_LINE_LENGTH * LOG_QUEUE_LENGTH)

static StaticTask_t log_task_static;
static StackType_t log_stack[LOG_STACK_SIZE];

static StaticMessageBuffer_t log_mb_static;
static MessageBufferHandle_t log_mb;
static uint8_t log_mb_storage[LOG_MB_SIZE];

static volatile bool enabled = false;

static void cec_log_task(void *param) {
  log_callback_t log = param;

  while (true) {
    char buffer[LOG_LINE_LENGTH];

    size_t bytes = xMessageBufferReceive(log_mb, buffer, sizeof(buffer), pdMS_TO_TICKS(10));
    if (bytes > 0) {
      log(buffer);
    }
  }
}

void cec_log_init(log_callback_t log) {
  log_mb = xMessageBufferCreateStatic(LOG_MB_SIZE, &log_mb_storage[0], &log_mb_static);
  enabled = false;

  xTaskCreateStatic(cec_log_task, LOG_TASK_NAME, LOG_STACK_SIZE, log, LOG_PRIORITY, &log_stack[0],
                    &log_task_static);
}

bool cec_log_enabled(void) {
  return enabled;
}

void cec_log_enable(void) {
  enabled = true;
}

void cec_log_disable(void) {
  enabled = false;
}

void cec_log_vsubmitf(const char *fmt, va_list ap) {
  if (enabled) {
    char buffer[LOG_LINE_LENGTH];

    int bytes = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    if (bytes >= 0 && (size_t)bytes < sizeof(buffer)) {
      xMessageBufferSend(log_mb, buffer, (size_t)bytes + 1, 0);
    }
  }
}

void cec_log_submitf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  cec_log_vsubmitf(fmt, ap);
  va_end(ap);
}

const char *cec_message[] = {
    [CEC_ID_FEATURE_ABORT] = "Feature Abort",
    [CEC_ID_IMAGE_VIEW_ON] = "Image View On",
    [CEC_ID_TEXT_VIEW_ON] = "Text View On",
    [CEC_ID_STANDBY] = "Standby",
    [CEC_ID_USER_CONTROL_PRESSED] = "User Control Pressed",
    [CEC_ID_USER_CONTROL_RELEASED] = "User Control Released",
    [CEC_ID_GIVE_OSD_NAME] = "Give OSD Name",
    [CEC_ID_SET_OSD_NAME] = "Set OSD Name",
    [CEC_ID_SYSTEM_AUDIO_MODE_REQUEST] = "System Audio Mode Request",
    [CEC_ID_GIVE_AUDIO_STATUS] = "Give Audio Status",
    [CEC_ID_SET_SYSTEM_AUDIO_MODE] = "Set System Audio Mode",
    [CEC_ID_GIVE_SYSTEM_AUDIO_MODE_STATUS] = "Give System Audio Mode",
    [CEC_ID_SYSTEM_AUDIO_MODE_STATUS] = "System Audio Mode Status",
    [CEC_ID_REPORT_AUDIO_STATUS] = "Report Audio Status",
    [CEC_ID_ROUTING_CHANGE] = "Routing Change",
    [CEC_ID_ROUTING_INFORMATION] = "Routing Information",
    [CEC_ID_ACTIVE_SOURCE] = "Active Source",
    [CEC_ID_GIVE_PHYSICAL_ADDRESS] = "Give Physical Address",
    [CEC_ID_REPORT_PHYSICAL_ADDRESS] = "Report Physical Address",
    [CEC_ID_REQUEST_ACTIVE_SOURCE] = "Request Active Source",
    [CEC_ID_SET_STREAM_PATH] = "Set Stream Path",
    [CEC_ID_DEVICE_VENDOR_ID] = "Device Vendor ID",
    [CEC_ID_GIVE_DEVICE_VENDOR_ID] = "Give Device Vendor ID",
    [CEC_ID_MENU_STATUS] = "Menu Status",
    [CEC_ID_MENU_REQUEST] = "Menu Request",
    [CEC_ID_GIVE_DEVICE_POWER_STATUS] = "Give Device Power Status",
    [CEC_ID_REPORT_POWER_STATUS] = "Report Power Status",
    [CEC_ID_GET_MENU_LANGUAGE] = "Get Menu Language",
    [CEC_ID_INACTIVE_SOURCE] = "Inactive Source",
    [CEC_ID_CEC_VERSION] = "CEC Version",
    [CEC_ID_GET_CEC_VERSION] = "Get CEC Version",
    [CEC_ID_VENDOR_COMMAND_WITH_ID] = "Vendor Command With ID",
    [CEC_ID_REQUEST_ARC_INITIATION] = "Request ARC Initiation",
    [CEC_ID_ABORT] = "Abort",
};

void cec_log_frame(cec_frame_t *frame, bool recv) {
  if (!enabled || !frame || !cec_message_valid(frame->message)) return;
  const cec_message_t *msg = frame->message;
  char raw[CEC_FRAME_MAX_LEN * 3 + 1];
  size_t used = 0;
  for (unsigned i = 0; i < msg->len; i++) {
    used += (size_t)snprintf(raw + used, sizeof(raw) - used,
                            i ? ":%02x" : "%02x", msg->data[i]);
  }
  const char *name = "Polling Message";
  if (msg->len > 1) {
    name = cec_message[msg->opcode];
    if (!name) name = "Unknown opcode";
  }
  cec_log_submitf("[%10llu] %s %s %s%s" _LOG_BR,
                  (unsigned long long)util_uptime_ms(), recv ? "RX" : "TX",
                  raw, name, frame->ack ? " ACK" : "");
}
