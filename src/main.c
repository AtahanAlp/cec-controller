#include "FreeRTOS.h"
#include "task.h"

#include "bsp/board.h"
#include "pico/stdlib.h"

#include "pico-cec/config.h"

#include "blink.h"
#include "cec-frame.h"
#include "cec-log.h"
#include "cec-task.h"
#include "usb-cdc.h"
#include "usb-device.h"
#include "ws2812.h"

int main() {
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

  vTaskStartScheduler();

  return 0;
}
