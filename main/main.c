/*
 * deskflow client - Main entry point
 *
 * Initializes Ethernet, TinyUSB HID, then creates and starts the
 * Barrier protocol client. Config and certs are loaded from USB stick
 * (or hardcoded defaults). The barrier_client_task handles TLS connection,
 * Barrier protocol packet dispatch, and actuator calls.
 */
#include "esp_log.h"
#include "driver/gpio.h"
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "config/config.h"
#include "button_reset.h"
#include "network/eth_network.h"
#include "network/network_events.h"
#include "tusb/tusb_device.h"
#include "usb_host/usb_host_msc.h"
#include "barrier/barrier_client.h"
#include "grove/grove_uart.h"
#include "web/web_server.h"
#include "led/led_rgb.h"

static const char *TAG = "deskflow_main";
#define MAIN_MONITOR_INTERVAL_MS  2000

/** Redirect ESP_LOG output to the web WebSocket log buffer. */
static int (*s_orig_vprintf)(const char *, va_list);

static int esp_log_ws_redirect(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);

    int r = 0;
    if (s_orig_vprintf && gpio_get_level(8))
        r = s_orig_vprintf(fmt, args);

    if (web_log_ws_connected()) {
        char buf[256];
        int len = vsnprintf(buf, sizeof(buf), fmt, copy);
        if (len > 0)
            web_log_push((const uint8_t *)buf, (size_t)len);
    }
    va_end(copy);
    return r;
}

/* Event group for tracking network state */
static EventGroupHandle_t s_network_events;

/* Ethernet state (global so event handlers can update flags) */
static eth_state_t s_eth_state;

void app_main(void)
{
    esp_err_t ret;

    /* Route ESP_LOG to the web debug-log WebSocket (/api/log/ws)
     * — falls through to UART when no WS client is connected. */
    s_orig_vprintf = esp_log_set_vprintf(esp_log_ws_redirect);

    ESP_LOGI(TAG, "deskflow client starting");

    /* --------------------------------------------------------
     * 0. Initialize GPIO8 as input
     * -------------------------------------------------------- */
    gpio_config_t gpio8_conf = {
        .pin_bit_mask = (1ULL << 8),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio8_conf);

    /* --------------------------------------------------------
     * 1. Initialize and start USB host MSC (runs permanently)
     * -------------------------------------------------------- */
    ret = usb_host_msc_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB host MSC init failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "USB host MSC infrastructure initialized (idle)");
        usb_host_msc_start();  // start and keep running
    }

    /* --------------------------------------------------------
     * 2. Initialize RGB LED (G15=Green, G16=Blue, G17=Red)
     * -------------------------------------------------------- */
    ret = led_rgb_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RGB LED init failed: %d", ret);
    }

    /* --------------------------------------------------------
     * 3. Initialize reset button (GPIO 45)
     * -------------------------------------------------------- */
    ret = button_reset_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Button reset init failed: %d", ret);
    }

    /* --------------------------------------------------------
     * 4. Load configuration and certs from USB
     * -------------------------------------------------------- */
    deskflow_config_t *cfg = config_get_current();
    config_load();

    ESP_LOGI(TAG, "Config: server=%s:%u name=%s screen=%ux%u jiggle=%us keep_awake=%d",
             cfg->server, cfg->port, cfg->device_name,
             cfg->screen_width, cfg->screen_height,
             cfg->jiggle_interval, cfg->keep_awake);

    if (usb_host_msc_is_mounted()) {
        ret = config_load_certs();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Certs not loaded (%s)", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "USB not mounted, certs not loaded");
    }

    /* --------------------------------------------------------
     * 4. Create event groups
     * -------------------------------------------------------- */
    s_network_events = xEventGroupCreate();
    if (s_network_events == NULL) {
        ESP_LOGE(TAG, "Failed to create network event group");
        abort();
    }

    /* --------------------------------------------------------
     * 5. Initialize Ethernet (persistent, not deinit pattern)
     * -------------------------------------------------------- */
    if (eth_init(&s_eth_state, s_network_events) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet init failed");
    } else if (eth_start(&s_eth_state) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed");
    } else {
        /* Apply VLAN from config (hot register writes, no restart) */
        eth_set_vlan(cfg->vlan_id);
        ESP_LOGI(TAG, "Ethernet started, waiting for IP...");
    }

    /* --------------------------------------------------------
     * 6. Initialize TinyUSB HID device
     * -------------------------------------------------------- */
    ret = tusb_device_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB init failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "TinyUSB device initialized");
    }

    /* --------------------------------------------------------
     * 7. Initialize Grove UART2 (G54=TX, G53=RX)
     * -------------------------------------------------------- */
    ret = grove_uart_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Grove UART init failed: %d", ret);
    }

    /* --------------------------------------------------------
     * 8. Start embedded HTTP settings server (port 80)
     * -------------------------------------------------------- */
    ret = web_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Web server start failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "Web settings server started (:80)");
    }
    web_server_set_event_group(s_network_events);

    /* --------------------------------------------------------
     * 9. Create and start Barrier client
     *
     * barrier_client_create() loads config, creates TLS client,
     * initializes actuator, and sets up reconnection logic.
     * barrier_client_start() creates the FreeRTOS task that runs
     * the Barrier protocol state machine.
     * -------------------------------------------------------- */
    barrier_client_t *client = barrier_client_create(NULL, s_network_events);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to create Barrier client");
    } else {
        ret = barrier_client_start(client);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Barrier client: %d", ret);
        } else {
            ESP_LOGI(TAG, "Barrier client task started");
        }
    }

    /* --------------------------------------------------------
     * 10. Main monitoring loop
     *
     * The app_main task becomes a lightweight monitor that logs
     * state changes. All protocol work is handled by the
     * barrier_client_task.
     * -------------------------------------------------------- */
    bool prev_has_ip = false;

    ESP_LOGI(TAG, "deskflow client running");

    while (true) {
        /* Block until an Ethernet event occurs, wake every 1s to update LED */
        uint32_t bits = xEventGroupWaitBits(s_network_events,
                                            EVENT_ETH_LINK_UP | EVENT_ETH_HAS_IP | EVENT_ETH_LINK_DOWN,
                                            pdFALSE,      /* don't clear bits */
                                            pdFALSE,      /* wait for any bit */
                                            pdMS_TO_TICKS(MAIN_MONITOR_INTERVAL_MS));

        bool has_ip    = !!(bits & EVENT_ETH_HAS_IP);

        if (has_ip != prev_has_ip) {
            if (has_ip) {
                ESP_LOGI(TAG, "Network: up (has IP)");
            } else {
                ESP_LOGI(TAG, "Network: down");
            }
            prev_has_ip = has_ip;
        }

        /* Update status LED */
        led_rgb_state(has_ip, tusb_device_is_mounted() || usb_host_msc_is_mounted());

        /* Drain any accumulated WebSocket log data */
        if (web_log_ws_connected()) {
            web_log_drain();
        }

        /* Always yield — wait bits returns instantly when bit is already set */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
