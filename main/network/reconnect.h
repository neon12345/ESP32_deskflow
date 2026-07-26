#ifndef RECONNECT_H
#define RECONNECT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int max_retries;
    uint32_t base_delay_ms;
    uint32_t max_delay_ms;
    uint32_t jitter_ms;
    int retry_count;
} reconnect_t;

void    reconnect_init(reconnect_t *rc);
void    reconnect_reset(reconnect_t *rc);
uint32_t reconnect_delay(reconnect_t *rc);
bool    reconnect_should_retry(reconnect_t *rc);

#endif
