/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USB_HOST_MSC_H
#define USB_HOST_MSC_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * Prepare USB host MSC subsystem.
 * Does NOT start the USB host stack — it stays inactive until usb_host_msc_start() is called.
 */
esp_err_t usb_host_msc_init(void);

/**
 * Start the USB host stack and wait for the MSC device to mount.
 * Call this on-demand when a USB mass storage device is needed.
 * Blocks until a device is mounted at /usb0 (or timeout).
 */
esp_err_t usb_host_msc_start(void);

/**
 * Unmount all MSC devices and tear down the USB host stack.
 * Call this when the USB mass storage device is no longer needed, or during shutdown.
 */
void usb_host_msc_stop(void);

/**
 * Return true if a MSC device is currently mounted.
 */
bool usb_host_msc_is_mounted(void);

#endif
