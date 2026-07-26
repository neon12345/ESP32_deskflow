#include "barrier_client.h"
#include "barrier_io.h"
#include "barrier_packet.h"

#include "tls_client.h"
#include "tusb_actuator.h"
#include "reconnect.h"
#include "config/config.h"
#include "network/network_events.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "barrier_client"

#define BARRIER_SERVER_HELLO      "Barrier"
#define BARRIER_SERVER_HELLO_LEN  7
#define BARRIER_CLIENT_MAJOR      1
#define BARRIER_CLIENT_MINOR      6
#define BARRIER_TASK_STACK        (4 * 1024)
#define BARRIER_TASK_PRIORITY     2  // Keep below IDLE feed threshold to avoid WDT
#define BARRIER_READ_BUF_SIZE     1024   /* Increased from 256 to handle larger TLS records */
#define BARRIER_WRITE_BUF_SIZE    1024   /* Increased from 256 for burst writes */
#define BARRIER_KEEPALIVE_TIMEOUT_MS  30000
#define BARRIER_HANDSHAKE_TIMEOUT_MS  5000
#define BARRIER_READ_POLL_MS          10
#define BARRIER_MAX_HELLO_LEN         64
#define BARRIER_HELLO_WITH_VERSION    (BARRIER_SERVER_HELLO_LEN + 4)
#define BARRIER_NETWORK_READY_MS      30000

static TaskHandle_t s_barrier_task = NULL;

TaskHandle_t barrier_client_get_task_handle(void)
{
    return s_barrier_task;
}

typedef enum {
    CLIENT_STATE_IDLE,
    CLIENT_STATE_CONNECTING,
    CLIENT_STATE_HANDSHAKE,
    CLIENT_STATE_CONNECTED,
    CLIENT_STATE_DISCONNECTED
} client_state_t;

typedef struct barrier_client {
    client_state_t state;
    tls_client_t *tls;
    tusb_actuator_t *actuator;
    reconnect_t reconnect;
    bool running;
    uint8_t read_buf[BARRIER_READ_BUF_SIZE];
    uint8_t write_buf[BARRIER_WRITE_BUF_SIZE];
    uint64_t last_activity_ms;
    bool cursor_entered;
    EventGroupHandle_t network_events;
    TaskHandle_t task_handle;  // For clean shutdown
} barrier_client_t;

static esp_err_t send_packet(barrier_client_t *client, packet_t *pkt)
{
    size_t out_len = BARRIER_WRITE_BUF_SIZE;
    esp_err_t err = packet_serialize(pkt, client->write_buf, &out_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "packet_serialize failed: %d", err);
        return err;
    }
    ssize_t sent = tls_client_write(client->tls, client->write_buf, out_len);
    if (sent < 0) {
        ESP_LOGE(TAG, "tls_client_write failed: %zd", sent);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t tls_read_full(barrier_client_t *client, uint8_t *buf, size_t len)
{
    size_t offset = 0;
    uint64_t start_ms = (uint64_t)(esp_timer_get_time() / 1000);
    uint32_t timeout_ms = BARRIER_HANDSHAKE_TIMEOUT_MS;  /* 5 s max for full handshake reads */

    while (offset < len) {
        ssize_t n = tls_client_read(client->tls, &buf[offset], len - offset);
        if (n > 0) {
            offset += (size_t)n;
        } else if (n == TLS_ERR_TIMEOUT) {
            /* Timeout on read — check wall clock */
            if (esp_timer_get_time() / 1000 - start_ms >= (int64_t)timeout_ms) {
                ESP_LOGE(TAG, "tls_read_full timeout (%u ms)", timeout_ms);
                return ESP_ERR_TIMEOUT;
            }
            uint32_t dummy;
            if (xTaskNotifyWait(0, 0, &dummy, pdMS_TO_TICKS(BARRIER_READ_POLL_MS)) == pdTRUE) {
                return ESP_FAIL;
            }
        } else {
            ESP_LOGE(TAG, "tls_read_full failed: %zd", n);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t handshake(barrier_client_t *client)
{
    /* 1. Connect TLS */
    esp_err_t err = tls_client_connect(client->tls);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TLS connect failed: %d", err);
        return err;
    }
    /* 2. Read server hello (length-prefixed: u32 BE length + payload) */
    err = tls_read_full(client, client->read_buf, 4);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Server hello length read failed");
        return ESP_FAIL;
    }
    uint32_t hello_len = io_read_u32_be(&client->read_buf[0]);
    if (hello_len < BARRIER_SERVER_HELLO_LEN || hello_len > 64) {
        ESP_LOGE(TAG, "Invalid server hello length: %u", hello_len);
        return ESP_FAIL;
    }

    /* Read the full payload: "Barrier" + version + possibly more */
    err = tls_read_full(client, client->read_buf, hello_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Server hello payload read failed");
        return ESP_FAIL;
    }
    if (memcmp(client->read_buf, BARRIER_SERVER_HELLO, BARRIER_SERVER_HELLO_LEN) != 0) {
        ESP_LOGE(TAG, "Invalid server hello: %.*s", (int)hello_len, client->read_buf);
        return ESP_FAIL;
    }
    /* 3. Extract version from hello payload (bytes after "Barrier") */
    if (hello_len >= BARRIER_SERVER_HELLO_LEN + 4) {
        uint16_t server_major = io_read_u16_be(&client->read_buf[BARRIER_SERVER_HELLO_LEN]);
        uint16_t server_minor = io_read_u16_be(&client->read_buf[BARRIER_SERVER_HELLO_LEN + 2]);
        (void)server_major; (void)server_minor;
    } else {
        /* Version not in hello payload — read it separately */
        err = tls_read_full(client, client->read_buf, 4);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Server version read failed");
            return ESP_FAIL;
        }
        uint16_t server_major = io_read_u16_be(&client->read_buf[0]);
        uint16_t server_minor = io_read_u16_be(&client->read_buf[2]);
        (void)server_major; (void)server_minor;
    }

    /* 4. Send client hello (match esparrier format) */
    const deskflow_config_t *cfg = config_get_current();
    const char *name = cfg->device_name;
    size_t name_len = strlen(name); /* esparrier does NOT include null terminator */

    size_t idx = 0;
    /* u32 total payload length (NOT including the length prefix itself) */
    io_write_u32_be(&client->write_buf[idx], (uint32_t)(BARRIER_SERVER_HELLO_LEN + 2 + 2 + 4 + name_len)); idx += 4;
    memcpy(&client->write_buf[idx], BARRIER_SERVER_HELLO, BARRIER_SERVER_HELLO_LEN); idx += BARRIER_SERVER_HELLO_LEN;
    io_write_u16_be(&client->write_buf[idx], BARRIER_CLIENT_MAJOR); idx += 2;
    io_write_u16_be(&client->write_buf[idx], BARRIER_CLIENT_MINOR); idx += 2;
    /* write_str: u32 length + raw bytes (no null terminator) */
    io_write_u32_be(&client->write_buf[idx], (uint32_t)name_len); idx += 4;
    memcpy(&client->write_buf[idx], name, name_len); idx += name_len;

    ssize_t sent = tls_client_write(client->tls, client->write_buf, idx);
    if (sent < 0) {
        ESP_LOGE(TAG, "Client hello write failed: %zd", sent);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t handle_packet(barrier_client_t *client, const packet_t *pkt)
{
    packet_t reply;
    const deskflow_config_t *cfg = config_get_current();

    switch (pkt->type) {
        case PACKET_QINF:
            /* Send device info - report PHYSICAL resolution.
             * Server tracks coords in physical pixels. HID deltas are physical.
             * Keep the chain 1:1 physical throughout. */
            memset(&reply, 0, sizeof(reply));
            reply.type = PACKET_DINF;
            reply.device_info.x     = 0;
            reply.device_info.y     = 0;
            reply.device_info.w     = cfg->screen_width;
            reply.device_info.h     = cfg->screen_height;
            reply.device_info.dummy = 0;
            reply.device_info.mx    = 0;
            reply.device_info.my    = 0;
            return send_packet(client, &reply);

        case PACKET_DINF:
            break;

        case PACKET_CALV:
            /* Respond with keep-alive */
            if (cfg->keep_awake) {
                esp_err_t act_ret = actuator_queue_jiggle(client->actuator);
                if (act_ret != ESP_OK) {
                    ESP_LOGW(TAG, "actuator_jiggle failed: %s", esp_err_to_name(act_ret));
                }
            }
            memset(&reply, 0, sizeof(reply));
            reply.type = PACKET_CALV;
            return send_packet(client, &reply);

        case PACKET_DMMV: {
            /* Movement -> shared state, no queue */
            actuator_set_movement(client->actuator, pkt->mouse_move_abs.x, pkt->mouse_move_abs.y);
            client->last_activity_ms = esp_timer_get_time() / 1000;
            break;
        }

        case PACKET_DMRM: {
            /* Movement -> shared state, no queue */
            actuator_move_relative(client->actuator, pkt->mouse_move_rel.dx, pkt->mouse_move_rel.dy);
            client->last_activity_ms = esp_timer_get_time() / 1000;
            break;
        }

        case PACKET_DKDN: {
            esp_err_t act_ret = actuator_queue_key_down(client->actuator, pkt->key_down.id, pkt->key_down.mask, pkt->key_down.button);
            if (act_ret != ESP_OK) return act_ret;
            client->last_activity_ms = esp_timer_get_time() / 1000;
            break;
        }

        case PACKET_DKUP: {
            esp_err_t act_ret = actuator_queue_key_up(client->actuator, pkt->key_up.id, pkt->key_up.mask, pkt->key_up.button);
            if (act_ret != ESP_OK) return act_ret;
            client->last_activity_ms = esp_timer_get_time() / 1000;
            break;
        }

        case PACKET_DMDN: {
            esp_err_t act_ret = actuator_queue_mouse_down(client->actuator, pkt->mouse_down.id);
            if (act_ret != ESP_OK) return act_ret;
            client->last_activity_ms = esp_timer_get_time() / 1000;
            break;
        }

        case PACKET_DMUP: {
            esp_err_t act_ret = actuator_queue_mouse_up(client->actuator, pkt->mouse_up.id);
            if (act_ret != ESP_OK) return act_ret;
            client->last_activity_ms = esp_timer_get_time() / 1000;
            break;
        }

        case PACKET_DMWM: {
            esp_err_t act_ret = actuator_queue_mouse_wheel(client->actuator, pkt->mouse_wheel.dx, pkt->mouse_wheel.dy);
            if (act_ret != ESP_OK) return act_ret;
            client->last_activity_ms = esp_timer_get_time() / 1000;
            break;
        }

        case PACKET_CINN: {
            client->cursor_entered = true;
            esp_err_t act_ret = actuator_queue_enter(client->actuator, pkt->cursor_enter.x, pkt->cursor_enter.y, pkt->cursor_enter.mask);
            if (act_ret != ESP_OK) return act_ret;
            client->last_activity_ms = esp_timer_get_time() / 1000;
            /* esparrier does NOT send CIAK in response to CINN */
            break;
        }

        case PACKET_COUT: {
            client->cursor_entered = false;
            esp_err_t act_ret = actuator_queue_leave(client->actuator);
            if (act_ret != ESP_OK) return act_ret;
            break;
        }

        case PACKET_CBYE:
        case PACKET_EUNK:
        case PACKET_EBSY:
        case PACKET_EBAD:
        case PACKET_EICV: {
            ESP_LOGW(TAG, "Server signaled disconnect: %s", packet_type_to_string(pkt->type));
            return ESP_FAIL;
        }

        case PACKET_CIAK:
        case PACKET_CNOP:
        case PACKET_CROP:
            break;

        case PACKET_DKRP: {
            esp_err_t act_ret = actuator_queue_key_repeat(client->actuator, pkt->key_repeat.id, pkt->key_repeat.mask, pkt->key_repeat.button, pkt->key_repeat.count);
            if (act_ret != ESP_OK) { ESP_LOGW(TAG, "actuator_key_repeat failed: %s", esp_err_to_name(act_ret)); }
            client->last_activity_ms = esp_timer_get_time() / 1000;

            break;
        }

        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t read_packet(barrier_client_t *client, packet_t *out, uint32_t timeout_ms)
{
    uint64_t start_ms = (uint64_t)(esp_timer_get_time() / 1000);

    /* Read 4-byte length prefix */
    uint8_t header[4];
    size_t offset = 0;

    while (offset < sizeof(header)) {
        ssize_t n = tls_client_read(client->tls, &header[offset], sizeof(header) - offset);
        if (n > 0) {
            offset += n;
        } else if (n == TLS_ERR_TIMEOUT) {
            /* Non-fatal timeout — check wall clock */
            if (esp_timer_get_time() / 1000 - start_ms >= (int64_t)timeout_ms) {
                return ESP_ERR_TIMEOUT;
            }
            uint32_t dummy;
            if (xTaskNotifyWait(0, 0, &dummy, pdMS_TO_TICKS(10)) == pdTRUE) {
                return ESP_FAIL;
            }
        } else {
            if (n == 0) {
                ESP_LOGW(TAG, "TLS read returned 0 (closed)");
                return ESP_FAIL;
            }
            ESP_LOGE(TAG, "TLS read failed: %zd", n);
            return ESP_FAIL;
        }
    }

    uint32_t payload_len = io_read_u32_be(header);

    if (payload_len > BARRIER_READ_BUF_SIZE - 4) {
        ESP_LOGE(TAG, "Payload too large: %u", payload_len);
        return ESP_ERR_INVALID_SIZE;
    }

    if (payload_len < 4) {
        ESP_LOGE(TAG, "Payload too small for packet code: %u", payload_len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Prepend length prefix then read payload so packet_parse sees full wire format */
    memcpy(&client->read_buf[0], header, sizeof(header));
    offset = 4;
    size_t total_len = 4 + payload_len;

    while (offset < total_len) {
        ssize_t n = tls_client_read(client->tls, &client->read_buf[offset], total_len - offset);
        if (n > 0) {
            offset += n;
        } else if (n == TLS_ERR_TIMEOUT) {
            /* Non-fatal timeout — check wall clock */
            if (esp_timer_get_time() / 1000 - start_ms >= (int64_t)timeout_ms) {
                return ESP_ERR_TIMEOUT;
            }
            uint32_t dummy;
            if (xTaskNotifyWait(0, 0, &dummy, pdMS_TO_TICKS(10)) == pdTRUE) {
                return ESP_FAIL;
            }
        } else {
            if (n == 0) {
                ESP_LOGW(TAG, "TLS read returned 0 (closed)");
                return ESP_FAIL;
            }
            ESP_LOGE(TAG, "TLS read failed: %zd", n);
            return ESP_FAIL;
        }
    }

    return packet_parse(client->read_buf, total_len, out);
}

static void barrier_client_task(void *arg)
{
    barrier_client_t *client = (barrier_client_t *)arg;
    client->running = true;
    client->last_activity_ms = esp_timer_get_time() / 1000;

    while (client->running) {
        /* Wait for network to be ready before attempting connection */
        if (client->network_events != NULL) {
            while (client->running) {
                EventBits_t bits = xEventGroupWaitBits(
                    client->network_events,
                    EVENT_ETH_HAS_IP,
                    pdFALSE, /* don't clear — main loop also reads this */
                    pdFALSE, /* don't wait for all */
                    pdMS_TO_TICKS(1000)
                );
                if (bits & EVENT_ETH_HAS_IP) {
                    break;
                }
                ESP_LOGW(TAG, "Network not ready (timeout), will retry");
            }
        }

        /* Check if config was changed via web server */
        if (client->network_events != NULL) {
            EventBits_t bits = xEventGroupGetBits(client->network_events);
            if (bits & EVENT_RECONNECT) {
                xEventGroupClearBits(client->network_events, EVENT_RECONNECT);
                const deskflow_config_t *new_cfg = config_get_current();
                if (new_cfg) {
                    actuator_queue_set_screen_size(client->actuator, new_cfg->screen_width, new_cfg->screen_height);
                    actuator_queue_set_scaling(client->actuator, new_cfg->scaling);
                    /* Always recreate TLS client - it may need new server/port/certs */
                    tls_client_destroy(client->tls);
                    client->tls = tls_client_create(new_cfg->server, new_cfg->port);
                    if (client->tls == NULL) {
                        ESP_LOGE(TAG, "Failed to recreate TLS client");
                        reconnect_reset(&client->reconnect);
                        uint32_t delay_ms = reconnect_delay(&client->reconnect);
                        vTaskDelay(pdMS_TO_TICKS(delay_ms));
                        continue;
                    }
                }
                tls_client_close(client->tls);
                continue;
            }
        }

        /* --- Connection loop --- */
        client->state = CLIENT_STATE_HANDSHAKE;

        esp_err_t err = handshake(client);
        if (err != ESP_OK) {
            client->state = CLIENT_STATE_DISCONNECTED;
            ESP_LOGW(TAG, "Handshake failed, reconnecting...");

            if (!reconnect_should_retry(&client->reconnect)) {
                ESP_LOGE(TAG, "Max retries exceeded");
                break;
            }
            uint32_t delay_ms = reconnect_delay(&client->reconnect);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            tls_client_close(client->tls);
            continue;
        }

        client->state = CLIENT_STATE_CONNECTED;
        reconnect_reset(&client->reconnect);
        actuator_queue_connected(client->actuator);

        /* --- Main packet loop --- */
        const deskflow_config_t *cfg = config_get_current();
        uint32_t timeout_ms = cfg->jiggle_interval * 1000;
        if (timeout_ms == 0) {
            timeout_ms = UINT32_MAX; /* No timeout if jiggle_interval is 0 */
        }

        while (client->running && client->state == CLIENT_STATE_CONNECTED) {
            packet_t pkt;
            cfg = config_get_current();
            err = read_packet(client, &pkt, timeout_ms);

            if (err != ESP_OK) {
                if (err == ESP_ERR_TIMEOUT) {
                    if (cfg->keep_awake && client->cursor_entered) {
                        uint64_t now = (uint64_t)(esp_timer_get_time() / 1000); /* ms */
                        if ((now - client->last_activity_ms) > cfg->jiggle_interval * 1000) {
                            actuator_queue_jiggle(client->actuator);
                            client->last_activity_ms = now;
                        }
                    }
                    continue;
                }

                /* Real error - disconnect */
                ESP_LOGE(TAG, "Read error: %d, disconnecting", err);
                break;
            }

            /* Check for config change — break out to reconnect */
            if (client->network_events != NULL) {
                EventBits_t bits = xEventGroupGetBits(client->network_events);
                if (bits & EVENT_RECONNECT) {
                    xEventGroupClearBits(client->network_events, EVENT_RECONNECT);
                    break;
                }
            }

            err = handle_packet(client, &pkt);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "handle_packet error: %s", esp_err_to_name(err));
                break;
            }
        }

        /* Disconnected - cleanup */
        client->state = CLIENT_STATE_DISCONNECTED;
        client->cursor_entered = false;
        actuator_queue_disconnected(client->actuator);
        tls_client_close(client->tls);
        ESP_LOGW(TAG, "Disconnected from server");

        if (!client->running) {
            break;
        }

        /* Reconnect delay */
        if (!reconnect_should_retry(&client->reconnect)) {
            ESP_LOGE(TAG, "Max retries exceeded, giving up");
            break;
        }
        uint32_t delay_ms = reconnect_delay(&client->reconnect);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    client->state = CLIENT_STATE_IDLE;
    client->running = false;
    vTaskDelete(NULL);
}

barrier_client_t *barrier_client_create(const deskflow_config_t *config,
                                        EventGroupHandle_t network_events)
{
    barrier_client_t *client = calloc(1, sizeof(barrier_client_t));
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to allocate client");
        return NULL;
    }

    /* Use config_get_current() for live config at runtime */
    const deskflow_config_t *live_cfg = config_get_current();
    if (live_cfg == NULL) {
        ESP_LOGE(TAG, "No config available");
        free(client);
        return NULL;
    }

    client->tls = tls_client_create(live_cfg->server, live_cfg->port);
    if (client->tls == NULL) {
        ESP_LOGE(TAG, "Failed to create TLS client");
        free(client);
        return NULL;
    }

    client->actuator = calloc(1, sizeof(tusb_actuator_t));
    if (client->actuator == NULL) {
        ESP_LOGE(TAG, "Failed to allocate actuator");
        tls_client_destroy(client->tls);
        free(client);
        return NULL;
    }

    if (tusb_actuator_init(client->actuator, live_cfg->screen_width, live_cfg->screen_height) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init actuator");
        free(client->actuator);
        tls_client_destroy(client->tls);
        free(client);
        return NULL;
    }

    if (actuator_queue_set_screen_size(client->actuator, live_cfg->screen_width, live_cfg->screen_height) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue screen size");
    }
    if (actuator_queue_set_scaling(client->actuator, live_cfg->scaling) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue scaling");
    }

    /* Start HID task on Core 1 */
    if (actuator_hid_task_start(client->actuator) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HID task");
        actuator_hid_task_stop(client->actuator);  /* frees queue */
        free(client->actuator);
        tls_client_destroy(client->tls);
        free(client);
        return NULL;
    }

    reconnect_init(&client->reconnect);
    client->state = CLIENT_STATE_IDLE;
    client->running = false;
    client->cursor_entered = false;
    client->last_activity_ms = 0;
    client->network_events = network_events;

    return client;
}

esp_err_t barrier_client_start(barrier_client_t *client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (client->running) {
        ESP_LOGW(TAG, "Client already running");
        return ESP_ERR_INVALID_STATE;
    }

    client->task_handle = NULL;
    BaseType_t xReturned = xTaskCreatePinnedToCore(
        barrier_client_task,
        "barrier_client",
        BARRIER_TASK_STACK,
        client,
        BARRIER_TASK_PRIORITY,
        &client->task_handle,
        0  /* Core 0 - network task (HID already on Core 1) */
    );

    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create barrier client task");
        return ESP_FAIL;
    }
    s_barrier_task = client->task_handle;
    return ESP_OK;
}

void barrier_client_stop(barrier_client_t *client)
{
    if (client == NULL) {
        return;
    }
    s_barrier_task = NULL;
    client->running = false;

    if (client->task_handle != NULL) {
        xTaskNotifyGive(client->task_handle);
        vTaskDelay(pdMS_TO_TICKS(500));  // wait for task to finish
        client->task_handle = NULL;
    }
}

void barrier_client_destroy(barrier_client_t *client)
{
    if (client == NULL) {
        return;
    }
    barrier_client_stop(client);
    actuator_hid_task_stop(client->actuator);
    tls_client_destroy(client->tls);
    client->tls = NULL;
    free(client->actuator);
    free(client);
}


