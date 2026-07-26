#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include "freertos/event_groups.h"

/**
 * Start the embedded HTTP settings server on port 80.
 */
esp_err_t web_server_start(void);

/** Provide the network event group so the web server can signal cert changes. */
void web_server_set_event_group(EventGroupHandle_t events);

#endif
