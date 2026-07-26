/*
 * TLS client via esp-tls (ESP-IDF v6.0.2).
 *
 * Uses certificate buffers managed by config module.
 * Certs are loaded at boot from USB and can be updated via web server.
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_tls.h"

#include "tls_client.h"
#include "config/config.h"

static const char *TAG = "tls_client";

#define TLS_TIMEOUT_MS          10000
#define TCP_KEEPALIVE_IDLE      30
#define TCP_KEEPALIVE_INTERVAL  10
#define TCP_KEEPALIVE_COUNT     3
#define TLS_CONN_SUCCESS        1
#define TLS_WRITE_RETRY_DELAY_MS  10
#define TLS_WRITE_MAX_RETRIES     30   // 30 * 10ms = 300ms max per write

struct tls_client {
    char        *server;
    int          port;
    bool         connected;
    esp_tls_t   *tls;
};

tls_client_t *tls_client_create(const char *server, int port)
{
    if (!server)
        return NULL;
    tls_client_t *client = calloc(1, sizeof(*client));
    if (!client) {
        ESP_LOGE(TAG, "calloc failed");
        return NULL;
    }
    client->server    = strdup(server);
    client->port      = port;
    client->connected = false;
    client->tls       = NULL;

    ESP_LOGI(TAG, "TLS client created for %s:%d", server, port);
    return client;
}

esp_err_t tls_client_connect(tls_client_t *client)
{
    if (!client)
        return ESP_ERR_INVALID_ARG;

    if (client->connected)
        tls_client_close(client);

    const unsigned char *ca      = config_get_ca();
    const unsigned char *cert    = config_get_client_cert();
    const unsigned char *key     = config_get_client_key();

    if (ca == NULL) {
        ESP_LOGE(TAG, "No CA cert loaded, cannot connect");
        return ESP_ERR_NOT_FOUND;
    }

    if (cert == NULL || key == NULL) {
        ESP_LOGE(TAG, "No client cert/key loaded, cannot connect");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Loading certs: CA(%u) client(%u) key(%u)",
             config_get_ca_len(), config_get_client_cert_len(), config_get_client_key_len());

    tls_keep_alive_cfg_t ka_cfg = {
        .keep_alive_enable   = true,
        .keep_alive_idle     = TCP_KEEPALIVE_IDLE,
        .keep_alive_interval = TCP_KEEPALIVE_INTERVAL,
        .keep_alive_count    = TCP_KEEPALIVE_COUNT,
    };

    esp_tls_cfg_t cfg = {
        .timeout_ms       = TLS_TIMEOUT_MS,
        .keep_alive_cfg   = &ka_cfg,
        .skip_common_name = true,
        .cacert_buf       = ca,
        .cacert_bytes     = config_get_ca_len(),
        .clientcert_buf   = cert,
        .clientcert_bytes = config_get_client_cert_len(),
        .clientkey_buf    = key,
        .clientkey_bytes  = config_get_client_key_len(),
    };

    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        ESP_LOGE(TAG, "esp_tls_init failed");
        return ESP_FAIL;
    }

    int ret = esp_tls_conn_new_sync(
                  client->server,
                  (int)strlen(client->server),
                  client->port,
                  &cfg,
                  tls);

    if (ret != TLS_CONN_SUCCESS) {
        esp_tls_error_handle_t err_handle;
        esp_tls_get_error_handle(tls, &err_handle);
        ESP_LOGE(TAG, "esp_tls_conn_new_sync failed: %d", err_handle->last_error);
        esp_tls_conn_destroy(tls);
        return ESP_FAIL;
    }

    client->tls       = tls;
    client->connected = true;
    ESP_LOGI(TAG, "TLS connected to %s:%d", client->server, client->port);
    return ESP_OK;
}

ssize_t tls_client_read(tls_client_t *client, void *buf, size_t len)
{
    if (!client || !client->connected || !client->tls)
        return -1;
    if (buf == NULL)
        return -1;

    ssize_t n = esp_tls_conn_read(client->tls, buf, len);

    if (n == 0) {
        esp_tls_conn_destroy(client->tls);
        client->tls = NULL;
        client->connected = false;
        ESP_LOGW(TAG, "Server closed TLS connection");
        return 0;
    }
    if (n < 0) {
        esp_tls_error_handle_t err_handle;
        esp_tls_get_error_handle(client->tls, &err_handle);
        esp_err_t last_err = esp_tls_get_and_clear_last_error(err_handle, NULL, NULL);

        if (last_err == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT) {
            ESP_LOGD(TAG, "TLS read timeout");
            return TLS_ERR_TIMEOUT;
        }
        esp_tls_conn_destroy(client->tls);
        client->tls = NULL;
        client->connected = false;
        return -1;
    }
    return n;
}

ssize_t tls_client_write(tls_client_t *client, const void *buf, size_t len)
{
    if (!client || !client->connected || !client->tls)
        return -1;
    if (buf == NULL)
        return -1;

    size_t offset = 0;
    int retries = 0;
    while (offset < len) {
        ssize_t n = esp_tls_conn_write(client->tls,
                                       (const unsigned char *)buf + offset,
                                       len - offset);
        if (n > 0) {
            offset += (size_t)n;
            retries = 0;  // reset on progress
        } else if (n == 0) {
            client->connected = false;
            ESP_LOGW(TAG, "Server closed TLS connection during write");
            return (ssize_t)offset;
        } else {
            esp_tls_error_handle_t err_handle;
            esp_tls_get_error_handle(client->tls, &err_handle);
            esp_err_t last_err = esp_tls_get_and_clear_last_error(err_handle, NULL, NULL);

            if (last_err == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT) {
                retries++;
                if (retries >= TLS_WRITE_MAX_RETRIES) {
                    ESP_LOGE(TAG, "TLS write timeout after %d retries, aborting", retries);
                    esp_tls_conn_destroy(client->tls);
                    client->tls = NULL;
                    client->connected = false;
                    return -1;
                }
                vTaskDelay(pdMS_TO_TICKS(TLS_WRITE_RETRY_DELAY_MS));
                continue;
            }
            ESP_LOGE(TAG, "TLS write error: %d", last_err);
            esp_tls_conn_destroy(client->tls);
            client->tls = NULL;
            client->connected = false;
            return (ssize_t)offset;
        }
    }
    return (ssize_t)len;
}

void tls_client_close(tls_client_t *client)
{
    if (!client)
        return;

    if (client->tls) {
        esp_tls_conn_destroy(client->tls);
        client->tls = NULL;
    }
    client->connected = false;
}

void tls_client_destroy(tls_client_t *client)
{
    if (!client)
        return;

    tls_client_close(client);
    free(client->server);
    free(client);
}
