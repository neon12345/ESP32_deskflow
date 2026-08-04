/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TUSB_DEVICE_H
#define TUSB_DEVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Our HID report IDs - namespaced to avoid TinyUSB enum clashes
#define DF_HID_KEYBOARD    1
#define DF_HID_ABS_MOUSE   4  // Absolute Mouse: buttons + X(16) + Y(16) + wheel

esp_err_t tusb_device_init(void);
bool      tusb_device_is_mounted(void);
bool      tusb_device_is_suspended(void);
bool      tusb_try_remote_wakeup(void);
void      tusb_set_pending_send(bool pending);

/* Set HID task handle so tud_hid_report_complete_cb can wake it */
void      tusb_set_hid_task_handle(void *handle);

void tusb_device_deinit(void);

#endif
