#ifndef BUTTON_RESET_H
#define BUTTON_RESET_H

/**
 * @brief Initialize the user button (GPIO 45) to trigger a system reset on press.
 *
 * The button is configured with a pull-up and generates an interrupt on
 * the falling edge. A short press triggers esp_restart().
 *
 * @return ESP_OK on success, or an esp_err_t on failure.
 */
esp_err_t button_reset_init(void);

/**
 * @brief Deinitialize the reset button and remove the GPIO ISR handler.
 */
void button_reset_deinit(void);

#endif /* BUTTON_RESET_H */
