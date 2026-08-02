/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/event_groups.h"

/* Start the embedded HTTP/WS server */
esp_err_t web_server_start(void);

/* Provide the network event group so the web server can signal cert changes */
void      web_server_set_event_group(EventGroupHandle_t events);

/* Push bytes into the debug-log ring buffer (drained over /api/log/ws) */
void      web_log_push(const uint8_t *data, size_t len);

/* Returns true if a log WebSocket client is connected */
bool      web_log_ws_connected(void);

#endif
