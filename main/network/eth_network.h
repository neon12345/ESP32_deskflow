/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ETH_NETWORK_H
#define ETH_NETWORK_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"
#include "network_events.h"

typedef struct {
    esp_eth_handle_t eth_handle;
    esp_netif_t *eth_netif;
    esp_eth_netif_glue_handle_t glue;
    bool link_up;
    bool has_ip;
} eth_state_t;

esp_err_t eth_init(eth_state_t *state, EventGroupHandle_t event_group);
esp_err_t eth_start(eth_state_t *state);
void      eth_stop(eth_state_t *state);

#endif
