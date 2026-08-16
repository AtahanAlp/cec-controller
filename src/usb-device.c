/*
 * Derived from TinyUSB's FreeRTOS device example.
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 */

#include "bsp/board.h"
#include "tusb.h"

#include "usb-device.h"

void usb_task(void *param) {
  (void)param;

  tud_init(BOARD_TUD_RHPORT);
  while (true) {
    tud_task();
  }
}

void tud_mount_cb(void) {}

void tud_umount_cb(void) {}

void tud_suspend_cb(bool remote_wakeup_en) {
  (void)remote_wakeup_en;
}

void tud_resume_cb(void) {}
