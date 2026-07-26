#ifndef GROVE_UART_H
#define GROVE_UART_H

#include "esp_err.h"
#include <stddef.h>
#include "freertos/queue.h"

/**
 * Initialize UART2 on Grove port (G54=TX, G53=RX).
 */
esp_err_t grove_uart_init(void);

/**
 * Return the UART event queue handle so tasks can block on
 * UART_EVENT_RX_DATA instead of polling.
 */
QueueHandle_t grove_uart_get_event_queue(void);

/**
 * Read up to `len` bytes from Grove UART (non-blocking).
 * Returns number of bytes read, or 0 if nothing available.
 */
size_t grove_uart_read(void *buf, size_t len);

/**
 * Write `len` bytes to Grove UART.
 */
esp_err_t grove_uart_write(const void *data, size_t len);

/**
 * Deinitialize Grove UART and free resources.
 */
void grove_uart_deinit(void);

#endif
