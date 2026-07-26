#ifndef LED_RGB_H
#define LED_RGB_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Initialize RGB LED (common-anode, G15=Green, G16=Blue, G17=Red)
 *
 * @return ESP_OK on success
 */
esp_err_t led_rgb_init(void);

/**
 * @brief Update LED based on connection state
 *
 * Red    = nothing connected
 * Blue   = Ethernet only
 * Yellow = USB only
 * Green  = all connected
 *
 * @param eth_connected  Ethernet link + IP acquired
 * @param usb_connected  TinyUSB mounted
 */
void led_rgb_state(bool eth_connected, bool usb_connected);

/**
 * @brief Deinitialize the RGB LED and disable LEDC channels.
 */
void led_rgb_deinit(void);

#endif
