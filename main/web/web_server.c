/*
 * Embedded HTTP settings server
 *
 * Serves a settings page at / and provides a JSON API at /api/settings
 * for reading and writing the deskflow configuration.
 */
#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "driver/uart.h"
#include "config/config.h"
#include "grove/grove_uart.h"
#include "network/network_events.h"
#include "barrier/barrier_client.h"
#include "web_server.h"
#include "uart_page.h"

static const char *TAG = "web_server";

/* Event group for signaling cert changes to the barrier client */
static EventGroupHandle_t s_event_group = NULL;

/** Provide the network event group so the web server can signal cert changes. */
void web_server_set_event_group(EventGroupHandle_t events)
{
    s_event_group = events;
}

/* ------------------------------------------------------------------
 * Embedded HTML page
 * ------------------------------------------------------------------ */
static const char SETTINGS_PAGE[] =
"<!DOCTYPE html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>deskflow settings</title>"
"<style>"
"body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:16px}"
"form{max-width:400px;margin:0 auto}"
"label{display:block;margin-top:12px;font-size:13px;color:#aaa}"
"input,select{width:100%;padding:8px;margin-top:4px;border:none;border-radius:4px;"
"background:#222;color:#eee;font-size:14px;box-sizing:border-box}"
"button{margin-top:24px;width:100%;padding:12px;border:none;border-radius:6px;"
"background:#0a6;font-size:16px;cursor:pointer}"
"button:hover{background:#0b7}"
"button.danger{background:#a33;margin-top:12px}"
"button.danger:hover{background:#c44}"
"h1{font-size:20px;text-align:center;margin-bottom:8px}"
".row{display:flex;gap:8px}"
".row input{width:50%}"
"</style></head><body>"
"<h1>deskflow client</h1>"
"<form id='f'>"
"<label>Server</label>"
"<input id='server' name='server' required>"
"<label>Port</label>"
"<input id='port' type='number' name='port' required>"
"<label>Device name</label>"
"<input id='device_name' name='device_name'>"
"<label>Screen resolution</label>"
"<div class='row'>"
"<input id='width' type='number' name='screen_width'>"
"<input id='height' type='number' name='screen_height'>"
"</div>"
"<label>Display scaling</label>"
"<select id='scaling' name='scaling'>"
"<option value='100'>100%</option>"
"<option value='125'>125%</option>"
"<option value='150'>150%</option>"
"<option value='200'>200%</option>"
"</select>"
"<label>Jiggle interval (s)</label>"
"<input id='jiggle' type='number' name='jiggle_interval' min='0'>"
"<label><input type='checkbox' id='keep_awake' name='keep_awake' style='width:auto'>"
" Keep display awake</label>"
"<label>Keyboard layout</label>"
"<select id='keyboard_layout' name='keyboard_layout'>"
"<option value='0'>US</option>"
"<option value='1'>German (DE)</option>"
"</select>"
"<button type='submit'>Save</button>"
"<button type='button' class='danger' onclick='reboot()'>Reboot</button>"
"</form>"
"<script>"
"const load=()=>fetch('/api/settings').then(r=>r.json()).then(d=>{"
"document.getElementById('server').value=d.server||'';"
"document.getElementById('port').value=d.port||24800;"
"document.getElementById('device_name').value=d.device_name||'';"
"document.getElementById('width').value=d.screen_width||1920;"
"document.getElementById('height').value=d.screen_height||1080;"
"document.getElementById('scaling').value=d.scaling||100;"
"document.getElementById('jiggle').value=d.jiggle_interval||30;"
"document.getElementById('keep_awake').checked=!!d.keep_awake;"
"document.getElementById('keyboard_layout').value=d.keyboard_layout||0;"
"});"
"load();"
"document.getElementById('f').onsubmit=async e=>{"
"e.preventDefault();"
"const d={};new FormData(e.target).forEach((v,k)=>d[k]=v);"
"if(d.keep_awake===undefined) d.keep_awake=0; else d.keep_awake=1;"
"d.port=+d.port;d.screen_width=+d.screen_width;d.screen_height=+d.screen_height;"
"d.scaling=+d.scaling;d.jiggle_interval=+d.jiggle_interval;d.keyboard_layout=+d.keyboard_layout;"
"fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify(d)});load();"
"};"
"function reboot(){if(confirm('Reboot now?'))fetch('/api/reboot');}"
"</script>"
"<hr><label>Certificates (PEM)</label>"
"<label style='font-size:11px'>CA</label>"
"<textarea id='ca' style='width:100%;height:60px;background:#1a1a1a;color:#ccc;border:1px solid #333;padding:4px;font-size:11px'></textarea>"
"<label style='font-size:11px'>Client cert</label>"
"<textarea id='cert' style='width:100%;height:60px;background:#1a1a1a;color:#ccc;border:1px solid #333;padding:4px;font-size:11px'></textarea>"
"<label style='font-size:11px'>Client key</label>"
"<textarea id='key' style='width:100%;height:60px;background:#1a1a1a;color:#ccc;border:1px solid #333;padding:4px;font-size:11px'></textarea>"
"<button type='button' onclick='uploadCerts()'>Upload certs</button>"
"<script>"
"function uploadCerts(){"
"fetch('/api/cert',{method:'POST',headers:{'Content-Type':'application/json'}"
",body:JSON.stringify({"
"ca:document.getElementById('ca').value,"
"cert:document.getElementById('cert').value,"
"key:document.getElementById('key').value"
"})}).then(r=>{if(r.ok)alert('Certs uploaded. Reconnecting...');else alert('Upload failed');});"
"}"
"</script></body></html>";

/* ------------------------------------------------------------------
 * GET /  -> settings page
 * ------------------------------------------------------------------ */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, SETTINGS_PAGE, strlen(SETTINGS_PAGE));
}

static esp_err_t uart_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, UART_PAGE, strlen(UART_PAGE));
}

/* ------------------------------------------------------------------
 * GET /api/settings  -> current config as JSON
 * ------------------------------------------------------------------ */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    deskflow_config_t *cfg = config_get_current();
    if (cfg == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Config not loaded");
        return ESP_FAIL;
    }

    /* Build JSON string */
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"server\":\"%s\","
        "\"port\":%u,"
        "\"device_name\":\"%s\","
        "\"screen_width\":%u,"
        "\"screen_height\":%u,"
        "\"scaling\":%u,"
        "\"jiggle_interval\":%u,"
        "\"keep_awake\":%d,"
        "\"keyboard_layout\":%d}",
                cfg->server, cfg->port, cfg->device_name,
                cfg->screen_width, cfg->screen_height, cfg->scaling,
                cfg->jiggle_interval, cfg->keep_awake, cfg->keyboard_layout);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* ------------------------------------------------------------------
 * POST /api/settings  -> save config to USB
 * ------------------------------------------------------------------ */
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    int ret;
    char buf[1024];
    memset(buf, 0, sizeof(buf));

    if (req->content_len > (int)sizeof(buf) - 1) {
        ESP_LOGW(TAG, "settings payload too large (%d)", req->content_len);
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Payload too large");
        return ESP_OK;
    }

    ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_OK;
    }

    deskflow_config_t cfg = *config_get_current();

    const char *s;

    s = strstr(buf, "\"server\":\"");
    if (s) {
        s += sizeof("\"server\":\"") - 1;
        const char *e = strchr(s, '"');
        if (e) {
            int len = (int)(e - s);
            if (len < (int)sizeof(cfg.server)) {
                memcpy(cfg.server, s, len);
                cfg.server[len] = '\0';
            }
        }
    }

    s = strstr(buf, "\"device_name\":\"");
    if (s) {
        s += sizeof("\"device_name\":\"") - 1;
        const char *e = strchr(s, '"');
        if (e) {
            int len = (int)(e - s);
            if (len < (int)sizeof(cfg.device_name)) {
                memcpy(cfg.device_name, s, len);
                cfg.device_name[len] = '\0';
            }
        }
    }

    s = strstr(buf, "\"port\":");
    if (s) cfg.port = (uint16_t)strtoul(s + sizeof("\"port\":") - 1, NULL, 10);

    s = strstr(buf, "\"screen_width\":");
    if (s) cfg.screen_width = (uint16_t)strtoul(s + sizeof("\"screen_width\":") - 1, NULL, 10);

    s = strstr(buf, "\"screen_height\":");
    if (s) cfg.screen_height = (uint16_t)strtoul(s + sizeof("\"screen_height\":") - 1, NULL, 10);

    s = strstr(buf, "\"scaling\":");
    if (s) cfg.scaling = (uint16_t)strtoul(s + sizeof("\"scaling\":") - 1, NULL, 10);

    s = strstr(buf, "\"jiggle_interval\":");
    if (s) cfg.jiggle_interval = (uint16_t)strtoul(s + sizeof("\"jiggle_interval\":") - 1, NULL, 10);

    s = strstr(buf, "\"keep_awake\":");
    if (s) cfg.keep_awake = (int)strtoul(s + sizeof("\"keep_awake\":") - 1, NULL, 10);

    s = strstr(buf, "\"keyboard_layout\":");
    if (s) cfg.keyboard_layout = (uint8_t)strtoul(s + sizeof("\"keyboard_layout\":") - 1, NULL, 10);

    if (config_save(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Config saved to USB via web");

    /* Signal barrier client to reconnect with new config */
    if (s_event_group != NULL) {
        xEventGroupSetBits(s_event_group, EVENT_RECONNECT);
    }
    /* Wake barrier client from blocking calls */
    TaskHandle_t task = barrier_client_get_task_handle();
    if (task != NULL) {
        xTaskNotifyGive(task);
    }

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_sendstr(req, "\"saved\"");
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * POST /api/reboot
 * ------------------------------------------------------------------ */
static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "\"rebooting\"");
    ESP_LOGI(TAG, "Reboot requested via web");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * POST /api/cert  -> upload certificates to USB + RAM
 * ------------------------------------------------------------------ */
static esp_err_t cert_upload_handler(httpd_req_t *req)
{
    char buf[4096];
    if (req->content_len > (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Payload too large");
        return ESP_OK;
    }
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_OK;
    }
    buf[ret] = 0;

    /* Simple JSON field extraction */
    const char *s;
    esp_err_t r = ESP_OK;

    s = strstr(buf, "\"ca\":\"");
    if (s) {
        s += sizeof("\"ca\":\"") - 1;
        const char *e = strstr(s, "\",\"");
        if (e) {
            size_t len = (size_t)(e - s);
            if (len > 0) r = config_upload_cert(0, s, len);
        }
    }

    s = strstr(buf, "\"cert\":\"");
    if (s) {
        s += sizeof("\"cert\":\"") - 1;
        const char *e = strstr(s, "\",\"");
        if (e) {
            size_t len = (size_t)(e - s);
            if (len > 0) r = config_upload_cert(1, s, len);
        }
    }

    s = strstr(buf, "\"key\":\"");
    if (s) {
        s += sizeof("\"key\":\"") - 1;
        const char *e = strchr(s, '"');
        if (e) {
            size_t len = (size_t)(e - s);
            if (len > 0) r = config_upload_cert(2, s, len);
        }
    }

    if (r != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
        return ESP_FAIL;
    }

    /* Signal the barrier client that certs have changed so it can reconnect */
    if (s_event_group != NULL) {
        xEventGroupSetBits(s_event_group, EVENT_RECONNECT);
        ESP_LOGI(TAG, "Signaled EVENT_RECONNECT");
    }

    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * POST /api/uart/write  -> send bytes to Grove UART
 * ------------------------------------------------------------------ */
static esp_err_t uart_write_handler(httpd_req_t *req)
{
    int ret;
    char buf[4096];

    if (req->content_len > (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Payload too large");
        return ESP_OK;
    }

    ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_OK;
    }

    grove_uart_write(buf, (size_t)ret);

    char resp[32];
    int n = snprintf(resp, sizeof(resp), "{\"written\":%d}", ret);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, n);
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * POST /api/uart/read  -> read available bytes from Grove UART
 * ------------------------------------------------------------------ */
static esp_err_t uart_read_handler(httpd_req_t *req)
{
    uint8_t *buf = malloc(4096);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t bytes = grove_uart_read(buf, 4096);

    size_t resp_size = bytes * 5 + 32;
    char *resp = malloc(resp_size);
    if (!resp) { free(buf); return ESP_ERR_NO_MEM; }

    int n = snprintf(resp, resp_size, "{\"bytes\":[");
    for (size_t i = 0; i < bytes && n < (int)resp_size - 6; i++) {
        n += snprintf(resp + n, resp_size - (size_t)n, "%s%d",
                      i ? "," : "", buf[i]);
    }
    n += snprintf(resp + n, resp_size - (size_t)n, "],\"count\":%u}", (unsigned)bytes);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, n);
    free(resp);
    free(buf);
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * WebSocket /api/uart/ws  -> real-time bidirectional UART bridge
 *
 * ESP-IDF calls this handler ONCE PER INCOMING FRAME (not once per connection).
 * We use the two-step receive pattern from ESP-IDF examples:
 *   1. httpd_ws_recv_frame(req, &ws_pkt, 0)   -> get frame length
 *   2. httpd_ws_recv_frame(req, &ws_pkt, len) -> read payload
 *
 * Outgoing UART data is sent via httpd_queue_work + httpd_ws_send_frame_async.
 * ------------------------------------------------------------------ */

#ifdef CONFIG_HTTPD_WS_SUPPORT

/** Context shared between handler (per-frame) and async sender task. */
typedef struct {
    httpd_handle_t    server;       /* HTTPD server handle */
    int               fd;           /* WebSocket socket fd */
    volatile bool     running;
    TaskHandle_t      sender_task;
} uart_ws_ctx_t;

static uart_ws_ctx_t g_ws_ctx = {0};

typedef struct {
    httpd_handle_t hd;
    int            fd;
    uint8_t       *data;
    size_t         len;
} async_ws_send_arg_t;

/**
 * Async send callback: executed on the HTTPD thread via httpd_queue_work.
 * Sends a WebSocket frame using httpd_ws_send_frame_async.
 */
static void uart_ws_async_send(void *arg)
{
    async_ws_send_arg_t *a = (async_ws_send_arg_t *)arg;

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = a->data;
    ws_pkt.len     = a->len;
    ws_pkt.type    = HTTPD_WS_TYPE_BINARY;

    if (httpd_ws_send_frame_async(a->hd, a->fd, &ws_pkt) != ESP_OK) {
        ESP_LOGW(TAG, "ws_async_send: send failed");
    }
    free(a->data);
    free(a);
}

/**
 * Queue a WebSocket send via httpd_queue_work (runs on HTTPD thread).
 */
static esp_err_t uart_ws_queue_send(httpd_handle_t server, int fd,
                                     const uint8_t *data, size_t len)
{
    async_ws_send_arg_t *arg = calloc(1, sizeof(*arg));
    if (!arg) return ESP_ERR_NO_MEM;

    arg->data = malloc(len);
    if (!arg->data) { free(arg); return ESP_ERR_NO_MEM; }
    memcpy(arg->data, data, len);
    arg->len = len;
    arg->hd  = server;
    arg->fd  = fd;

    return httpd_queue_work(server, uart_ws_async_send, arg);
}

/**
 * Background task: reads UART and sends data via WebSocket (async).
 */
static void uart_ws_sender_task(void *arg)
{
    uart_ws_ctx_t *ctx = (uart_ws_ctx_t *)arg;
    QueueHandle_t uart_q = grove_uart_get_event_queue();
    uart_event_t event;
    uint8_t buf[2048];

    while (ctx->running) {
        if (xQueueReceive(uart_q, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (event.type == UART_DATA) {
            size_t bytes = grove_uart_read(buf, sizeof(buf));
            if (bytes > 0) {
                if (uart_ws_queue_send(ctx->server, ctx->fd, buf, bytes) != ESP_OK) {
                    ESP_LOGW(TAG, "ws_sender: queue_send failed, dropping");
                }
            }
        }
    }
    vTaskDelete(NULL);
}

/** Stop WebSocket sender task and clean up. */
static void uart_ws_cleanup(void)
{
    if (g_ws_ctx.sender_task) {
        g_ws_ctx.running = false;
        vTaskDelay(pdMS_TO_TICKS(50));
        g_ws_ctx.sender_task = NULL;
    }
    g_ws_ctx.server = NULL;
    g_ws_ctx.fd     = -1;
}

/**
 * WebSocket handler - called ONCE PER INCOMING FRAME by ESP-IDF.
 * Uses two-step receive pattern from ESP-IDF ws_echo_server example.
 */
static esp_err_t uart_ws_handler(httpd_req_t *req)
{
    /* First call: initialize context and start sender task */
    if (!g_ws_ctx.running) {
        uart_ws_cleanup(); /* safety: clear stale state */
        g_ws_ctx.server = req->handle;
        g_ws_ctx.fd     = httpd_req_to_sockfd(req);
        g_ws_ctx.running = true;

        if (xTaskCreate(uart_ws_sender_task, "uart_ws_s", 4096, &g_ws_ctx, 5,
                        &g_ws_ctx.sender_task) != pdPASS) {
            ESP_LOGE(TAG, "ws: failed to create sender task");
            g_ws_ctx.running = false;
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "ws: connection established fd=%d", g_ws_ctx.fd);
    }

    /* Two-step receive (ESP-IDF ws_echo_server pattern) */
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    /* Step 1: get frame length */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws: recv_frame(len) failed: %d, disconnecting", ret);
        uart_ws_cleanup();
        return ret;
    }

    /* Step 2: allocate and read payload */
    if (ws_pkt.len > 0) {
        buf = malloc(ws_pkt.len);
        if (!buf) {
            uart_ws_cleanup();
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ws: recv_frame(payload) failed: %d", ret);
            free(buf);
            uart_ws_cleanup();
            return ret;
        }

        /* Write received data to UART */
        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT || ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
            if (grove_uart_write(ws_pkt.payload, ws_pkt.len) != ESP_OK) {
                ESP_LOGW(TAG, "ws: uart_write failed");
            }
        }
        free(buf);
    }

    return ESP_OK;
}

#endif /* CONFIG_HTTPD_WS_SUPPORT */

/* ------------------------------------------------------------------
 * Server start
 * ------------------------------------------------------------------ */
esp_err_t web_server_start(void)
{
    httpd_handle_t server = NULL;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.ctrl_port = 32768;
    config.max_uri_handlers = 9;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t index  = { .uri = "/",           .method = HTTP_GET,  .handler = index_get_handler };
    httpd_uri_t get_s  = { .uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler };
    httpd_uri_t post_s = { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler };
    httpd_uri_t reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler };
    httpd_uri_t certs  = { .uri = "/api/cert",   .method = HTTP_POST, .handler = cert_upload_handler };

    httpd_uri_t uart_w = { .uri = "/api/uart/write", .method = HTTP_POST, .handler = uart_write_handler };
    httpd_uri_t uart_r = { .uri = "/api/uart/read",  .method = HTTP_POST, .handler = uart_read_handler };
    httpd_uri_t uart_p = { .uri = "/uart",           .method = HTTP_GET,  .handler = uart_page_handler };

    httpd_register_uri_handler(server, &index);
    httpd_register_uri_handler(server, &get_s);
    httpd_register_uri_handler(server, &post_s);
    httpd_register_uri_handler(server, &reboot);
    httpd_register_uri_handler(server, &certs);
    httpd_register_uri_handler(server, &uart_w);
    httpd_register_uri_handler(server, &uart_r);
    httpd_register_uri_handler(server, &uart_p);

#ifdef CONFIG_HTTPD_WS_SUPPORT
    httpd_uri_t uart_ws = { .uri = "/api/uart/ws",   .method = HTTP_GET,  .handler = uart_ws_handler, .user_ctx = NULL };
    uart_ws.is_websocket = true;
    httpd_register_uri_handler(server, &uart_ws);
#endif

    ESP_LOGI(TAG, "Web server started on port 80");
    return ESP_OK;
}
