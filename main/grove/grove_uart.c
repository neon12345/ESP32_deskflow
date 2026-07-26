/*
 * Grove UART driver
 *
 * Initializes UART2 on the HY2.0-4P Grove connector.
 * G54 = TX (yellow wire), G53 = RX (white wire)
 * Uses event queue so readers can block instead of polling.
 */
#include "esp_log.h"
#include "esp_err.h"
#include "driver/uart.h"
#include "freertos/queue.h"
#include "grove_uart.h"

static const char *TAG = "grove_uart";

#define GROVE_UART    UART_NUM_2
#define GROVE_TX_PIN  54
#define GROVE_RX_PIN  53
#define GROVE_BAUD    115200
#define GROVE_RX_BUF  2048
#define GROVE_TX_BUF  4096
#define GROVE_EVT_QD  4

static QueueHandle_t s_evt_queue = NULL;

esp_err_t grove_uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate    = GROVE_BAUD,
        .data_bits    = UART_DATA_8_BITS,
        .parity       = UART_PARITY_DISABLE,
        .stop_bits    = UART_STOP_BITS_1,
        .flow_ctrl    = UART_HW_FLOWCTRL_DISABLE,
        .source_clk   = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(GROVE_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(GROVE_UART, GROVE_TX_PIN, GROVE_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    s_evt_queue = xQueueCreate(GROVE_EVT_QD, sizeof(uart_event_t));
    ESP_ERROR_CHECK(uart_driver_install(GROVE_UART, GROVE_RX_BUF, GROVE_TX_BUF,
                                        GROVE_EVT_QD, &s_evt_queue, 0));

    ESP_LOGI(TAG, "UART2 @%u on G%d(TX)/G%d(RX)", GROVE_BAUD, GROVE_TX_PIN, GROVE_RX_PIN);
    return ESP_OK;
}

QueueHandle_t grove_uart_get_event_queue(void)
{
    return s_evt_queue;
}

size_t grove_uart_read(void *buf, size_t len)
{
    int bytes = uart_read_bytes(UART_NUM_2, buf, len, 0);
    return bytes >= 0 ? (size_t)bytes : 0;
}

esp_err_t grove_uart_write(const void *data, size_t len)
{
    int bytes = uart_write_bytes(UART_NUM_2, data, len);
    if (bytes <= 0) {
        ESP_LOGW(TAG, "UART2 write failed: asked=%zu got=%d", len, bytes);
        return ESP_FAIL;
    }
    if ((size_t)bytes < len) {
        ESP_LOGW(TAG, "partial write on UART2: asked=%zu got=%d", len, bytes);
    }
    return ESP_OK;
}

void grove_uart_deinit(void)
{
    uart_driver_delete(GROVE_UART);
    if (s_evt_queue != NULL) {
        vQueueDelete(s_evt_queue);
        s_evt_queue = NULL;
    }
}
