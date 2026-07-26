#include <esp_log.h>
#include <esp_system.h>
#include <esp_check.h>
#include <driver/gpio.h>

#include "button_reset.h"

static const char *TAG = "button_reset";

/* GPIO 45 = KEY_USER on the ESP32-P4 dev board */
#define BUTTON_RESET_GPIO       45

static void IRAM_ATTR button_reset_isr_handler(void *arg)
{
    (void)arg;
    esp_restart();
}

esp_err_t button_reset_init(void)
{
    esp_err_t ret;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_RESET_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    ret = gpio_config(&io_conf);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to configure button GPIO");

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install ISR service: %d", ret);
        return ret;
    }

    ret = gpio_isr_handler_add(BUTTON_RESET_GPIO, button_reset_isr_handler, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to add ISR handler");

    ESP_LOGI(TAG, "Reset button initialized (GPIO %d)", BUTTON_RESET_GPIO);
    return ESP_OK;
}

void button_reset_deinit(void)
{
    gpio_isr_handler_remove(BUTTON_RESET_GPIO);
}
