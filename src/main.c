#include "FreeRTOS.h"
#include "task.h"

#include "bsp/board.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "cec-health.h"

#include "pico-cec/config.h"

#include "blink.h"
#include "cec-frame.h"
#include "cec-log.h"
#include "cec-task.h"
#include "usb-cdc.h"
#include "usb-device.h"
#include "ws2812.h"

static void health_task(void *context) {
  (void)context;
  uint32_t boot = (uint32_t)(time_us_64() / 1000);
  while (true) {
    uint32_t now = (uint32_t)(time_us_64() / 1000);
    if (cec_health_fresh(now, boot, cec_heartbeat_ms(), cdc_heartbeat_ms())) {
      watchdog_update();
      if ((uint32_t)(now - boot) > 30000) watchdog_hw->scratch[1] = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

int main() {
  bool expired = watchdog_enable_caused_reboot();
  if (!expired || watchdog_hw->scratch[0] != 0x43454332) watchdog_hw->scratch[1] = 0;
  if (expired && ++watchdog_hw->scratch[1] >= 2) {
    watchdog_disable();
    gpio_init(CEC_PIN); gpio_disable_pulls(CEC_PIN); gpio_set_dir(CEC_PIN, GPIO_IN);
    /* Keep an enclosed unit recoverable over USB after an unexpected fault. */
    reset_usb_boot(0, 0);
  }
  watchdog_hw->scratch[0] = 0x43454332;
  watchdog_enable(8000, true);
  static StackType_t stackLED[LED_STACK_SIZE];
  static StackType_t stackCEC[CEC_STACK_SIZE];
  static StackType_t stackCDC[CDC_STACK_SIZE];
  static StackType_t stackUSB[USB_STACK_SIZE];

  static StaticTask_t xLEDTCB;
  static StaticTask_t xCECTCB;
  static StaticTask_t xUSBTCB;
  static StaticTask_t xCDCTCB;

  static TaskHandle_t xUSBTask;
  static TaskHandle_t xCDCTask;

  blink_init();

  stdio_init_all();
  board_init();

  alarm_pool_init_default();

  xBlinkTask = xTaskCreateStatic(blink_task, LED_TASK_NAME, LED_STACK_SIZE, NULL, LED_PRIORITY,
                                 &stackLED[0], &xLEDTCB);
  xCECTask = xTaskCreateStatic(cec_task, CEC_TASK_NAME, CEC_STACK_SIZE, NULL, CEC_PRIORITY,
                               &stackCEC[0], &xCECTCB);
  xUSBTask = xTaskCreateStatic(usb_task, USB_TASK_NAME, USB_STACK_SIZE, NULL, USB_PRIORITY,
                               &stackUSB[0], &xUSBTCB);
  xCDCTask = xTaskCreateStatic(cdc_task, CDC_TASK_NAME, CDC_STACK_SIZE, NULL, CDC_PRIORITY,
                               &stackCDC[0], &xCDCTCB);

  (void)xBlinkTask;
  (void)xCECTask;
  (void)xUSBTask;
  (void)xCDCTask;

  cec_log_init(cdc_log);

  static StackType_t health_stack[256];
  static StaticTask_t health_tcb;
  xTaskCreateStatic(health_task, "health", 256, NULL, 1, health_stack, &health_tcb);

  vTaskStartScheduler();

  return 0;
}
