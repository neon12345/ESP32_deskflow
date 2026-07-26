#include "led_rgb.h"

#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "led_rgb";

/* Common-anode RGB: G17=Red, G15=Green, G16=Blue */
#define LED_RGB_RED_PIN     17
#define LED_RGB_GREEN_PIN   15
#define LED_RGB_BLUE_PIN    16

#define LED_RGB_TIMER       LEDC_TIMER_0
#define LED_RGB_FREQ        1000
#define LED_RGB_RES         8  // 0-255

#define LED_RGB_CHANNEL_COUNT  3
#define LED_RGB_MAX_DUTY       255

esp_err_t led_rgb_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LED_RGB_TIMER,
        .freq_hz          = LED_RGB_FREQ,
        .duty_resolution  = LED_RGB_RES,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channels[] = {
        { .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .timer_sel = LED_RGB_TIMER, .gpio_num = LED_RGB_RED_PIN,   .duty = 0, .hpoint = 0 },
        { .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1, .timer_sel = LED_RGB_TIMER, .gpio_num = LED_RGB_GREEN_PIN, .duty = 0, .hpoint = 0 },
        { .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_2, .timer_sel = LED_RGB_TIMER, .gpio_num = LED_RGB_BLUE_PIN,  .duty = 0, .hpoint = 0 },
    };

    for (int i = 0; i < LED_RGB_CHANNEL_COUNT; i++) {
        ESP_ERROR_CHECK(ledc_channel_config(&channels[i]));
    }

    ESP_LOGI(TAG, "RGB LED initialized (G%d=G, G%d=B, G%d=R)",
             LED_RGB_GREEN_PIN, LED_RGB_BLUE_PIN, LED_RGB_RED_PIN);
    return ESP_OK;
}

/* Static helper: set raw RGB values */
static void led_rgb_set_raw(uint8_t r, uint8_t g, uint8_t b)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LED_RGB_MAX_DUTY - r);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, LED_RGB_MAX_DUTY - g);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, LED_RGB_MAX_DUTY - b);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
}

void led_rgb_state(bool eth_connected, bool usb_connected)
{
    uint8_t r, g, b;

    if (eth_connected && usb_connected) {
        r = 0; g = LED_RGB_MAX_DUTY; b = 0;              // green
    } else if (eth_connected) {
        r = 0; g = 0;   b = LED_RGB_MAX_DUTY;            // blue
    } else if (usb_connected) {
        r = LED_RGB_MAX_DUTY; g = LED_RGB_MAX_DUTY; b = 0; // yellow
    } else {
        r = LED_RGB_MAX_DUTY; g = 0; b = 0;              // red
    }

    led_rgb_set_raw(r, g, b);
}

void led_rgb_deinit(void)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0);
}
