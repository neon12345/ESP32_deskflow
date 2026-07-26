/*
 * Reconnection logic with exponential backoff and jitter
 *
 * Used for retrying TLS connections and network operations after failures.
 * Backoff strategy: base_delay * 2^retry_count, capped at max_delay,
 * with random jitter to avoid thundering herd on reconnect.
 */
#include "esp_random.h"
#include "reconnect.h"

#define RECONNECT_INFINITE_RETRIES  (-1)
#define RECONNECT_BASE_DELAY_MS     (1000U)
#define RECONNECT_MAX_DELAY_MS      (60000U)
#define RECONNECT_JITTER_MS         (5000U)

/**
 * @brief Initialize reconnection parameters with defaults
 *
 * Default values:
 * - max_retries: RECONNECT_INFINITE_RETRIES (infinite)
 * - base_delay_ms: RECONNECT_BASE_DELAY_MS (1 second)
 * - max_delay_ms: RECONNECT_MAX_DELAY_MS (60 seconds)
 * - jitter_ms: RECONNECT_JITTER_MS (5 seconds)
 */
void reconnect_init(reconnect_t *rc)
{
    rc->max_retries = RECONNECT_INFINITE_RETRIES;
    rc->base_delay_ms = RECONNECT_BASE_DELAY_MS;
    rc->max_delay_ms = RECONNECT_MAX_DELAY_MS;
    rc->jitter_ms = RECONNECT_JITTER_MS;
    rc->retry_count = 0;
}

/**
 * @brief Reset retry counter (call after successful connection)
 */
void reconnect_reset(reconnect_t *rc)
{
    rc->retry_count = 0;
}

/**
 * @brief Calculate delay for the next retry attempt
 *
 * Uses exponential backoff: base_delay * 2^retry_count
 * Capped at max_delay_ms, with random jitter added.
 * Increments the retry counter.
 *
 * @return Delay in milliseconds
 */
uint32_t reconnect_delay(reconnect_t *rc)
{
    /* Exponential backoff: base * 2^retry, capped at max */
    uint8_t shift = (rc->retry_count >= 30) ? 30 : (uint8_t)rc->retry_count;
    uint32_t delay = rc->base_delay_ms * (1U << shift);
    if (delay > rc->max_delay_ms) {
        delay = rc->max_delay_ms;
    }
    /* Add jitter to spread out reconnection attempts */
    if (rc->jitter_ms > 0) {
        delay += (uint32_t)(esp_random() % rc->jitter_ms);
    }
    rc->retry_count++;
    return delay;
}

/**
 * @brief Check if another retry is allowed
 *
 * @return true if retries are allowed, false if limit reached
 */
bool reconnect_should_retry(reconnect_t *rc)
{
    return rc->max_retries < 0 || rc->retry_count < rc->max_retries;
}
