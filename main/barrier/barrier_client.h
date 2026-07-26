#ifndef BARRIER_CLIENT_H
#define BARRIER_CLIENT_H

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "network/network_events.h"

/* Forward declaration - barrier_client.h does not need the full config type */
typedef struct deskflow_config deskflow_config_t;

typedef struct barrier_client barrier_client_t;

barrier_client_t *barrier_client_create(const deskflow_config_t *config,
                                        EventGroupHandle_t network_events);
esp_err_t barrier_client_start(barrier_client_t *client);
void      barrier_client_stop(barrier_client_t *client);
void      barrier_client_destroy(barrier_client_t *client);

/** Get task handle of the running barrier client (for external notification) */
TaskHandle_t barrier_client_get_task_handle(void);

#endif
