#ifndef TLS_CLIENT_H
#define TLS_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct tls_client tls_client_t;

/* Non-fatal timeout: no data within timeout_ms. Callers may retry. */
#define TLS_ERR_TIMEOUT   (-2)

tls_client_t *tls_client_create(const char *server, int port);
esp_err_t     tls_client_connect(tls_client_t *client);
ssize_t       tls_client_read(tls_client_t *client, void *buf, size_t len);
ssize_t       tls_client_write(tls_client_t *client, const void *buf, size_t len);
void          tls_client_close(tls_client_t *client);
void          tls_client_destroy(tls_client_t *client);

#endif
