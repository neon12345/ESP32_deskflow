#include "config.h"
#include "tusb/keycodes.h"
#include "esp_log.h"
#include "usb_host/usb_host_msc.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static const char *TAG = "config";

#define DEFAULT_SERVER        "192.168.1.10"
#define DEFAULT_PORT          24800
#define DEFAULT_DEVICE_NAME   "deskflow-client"
#define DEFAULT_KEEP_AWAKE    false
#define DEFAULT_JIGGLE_INT    30
#define DEFAULT_SCREEN_WIDTH  1920
#define DEFAULT_SCREEN_HEIGHT 1080
#define DEFAULT_SCALING       100
#define DEFAULT_KEYBOARD_LAYOUT 0  // 0=US, 1=DE
#define USB_SETTINGS_PATH     "/usb0/settings.json"
#define USB_CA_PATH           "/usb0/server_public_cert.pem"
#define USB_CLIENT_CERT_PATH  "/usb0/client_public_cert.pem"
#define USB_CLIENT_KEY_PATH   "/usb0/client_private_key.pem"

static deskflow_config_t s_config;

/* Certificate buffers (loaded at boot, updated via web) */
static unsigned char *s_ca_buf      = NULL;
static unsigned int   s_ca_len      = 0;
static unsigned char *s_client_cert = NULL;
static unsigned int   s_client_cert_len = 0;
static unsigned char *s_client_key  = NULL;
static unsigned int   s_client_key_len = 0;

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */
static void config_load_defaults(void)
{
    strncpy(s_config.server, DEFAULT_SERVER, sizeof(s_config.server) - 1);
    s_config.server[sizeof(s_config.server) - 1] = '\0';
    s_config.port = DEFAULT_PORT;
    strncpy(s_config.device_name, DEFAULT_DEVICE_NAME, sizeof(s_config.device_name) - 1);
    s_config.device_name[sizeof(s_config.device_name) - 1] = '\0';
    s_config.keep_awake = DEFAULT_KEEP_AWAKE;
    s_config.jiggle_interval = DEFAULT_JIGGLE_INT;
    s_config.screen_width = DEFAULT_SCREEN_WIDTH;
    s_config.screen_height = DEFAULT_SCREEN_HEIGHT;
    s_config.scaling = DEFAULT_SCALING;
    s_config.keyboard_layout = DEFAULT_KEYBOARD_LAYOUT;
}

/* ------------------------------------------------------------------ */
/* Save to USB stick                                                   */
/* ------------------------------------------------------------------ */
esp_err_t config_save(deskflow_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;

    if (!usb_host_msc_is_mounted()) {
        ESP_LOGW(TAG, "USB not mounted, cannot save");
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(USB_SETTINGS_PATH, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for writing", USB_SETTINGS_PATH);
        return ESP_FAIL;
    }

    fprintf(f,
        "{ \"server\": \"%s\",\n"
        " \"port\": %u,\n"
        " \"device_name\": \"%s\",\n"
        " \"screen_width\": %u,\n"
        " \"screen_height\": %u,\n"
        " \"scaling\": %u,\n"
        " \"jiggle_interval\": %u,\n"
        " \"keep_awake\": %s,\n"
        " \"keyboard_layout\": %u}",
        cfg->server, cfg->port, cfg->device_name,
        cfg->screen_width, cfg->screen_height,
        cfg->scaling,
        cfg->jiggle_interval, cfg->keep_awake ? "true" : "false",
        cfg->keyboard_layout);

    fflush(f);
    fsync(fileno(f));   /* flush FatFS cache to USB device */
    fclose(f);
    s_config = *cfg;

    /* Apply keyboard layout immediately */
    keycodes_set_layout(s_config.keyboard_layout);
    ESP_LOGI(TAG, "Config saved to %s, layout: %s", USB_SETTINGS_PATH,
             s_config.keyboard_layout == 1 ? "DE" : "US");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Load from USB stick (fallback to defaults)                          */
/* ------------------------------------------------------------------ */
esp_err_t config_load(void)
{
    config_load_defaults();

    /* Try loading from USB stick (caller must ensure USB is mounted) */
    if (!usb_host_msc_is_mounted()) {
        ESP_LOGI(TAG, "USB not mounted, using defaults");
        return ESP_OK;
    }

    FILE *f = fopen(USB_SETTINGS_PATH, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "No %s found on USB, using defaults", USB_SETTINGS_PATH);
        return ESP_OK;
    }

    /* Simple JSON parsing using fscanf */
    char server[64] = {0};
    char device_name[32] = {0};
    char keep_str[8] = {0};
    int port, sw, sh, sc, ji, kl;

    if (fscanf(f,
        "{ \"server\": \"%63[^\"]\",\n"
        " \"port\": %d,\n"
        " \"device_name\": \"%31[^\"]\",\n"
        " \"screen_width\": %d,\n"
        " \"screen_height\": %d,\n"
        " \"scaling\": %d,\n"
        " \"jiggle_interval\": %d,\n"
        " \"keep_awake\": %7[^,\n],\n"
        " \"keyboard_layout\": %d",
        server, &port, device_name, &sw, &sh, &sc, &ji, keep_str, &kl) == 9) {

        strncpy(s_config.server, server, sizeof(s_config.server) - 1);
        s_config.port = (uint16_t)port;
        strncpy(s_config.device_name, device_name, sizeof(s_config.device_name) - 1);
        s_config.screen_width = (uint16_t)sw;
        s_config.screen_height = (uint16_t)sh;
        s_config.scaling = (uint16_t)sc;
        s_config.jiggle_interval = (uint16_t)ji;
        s_config.keep_awake = (strcmp(keep_str, "true") == 0);
        s_config.keyboard_layout = (uint8_t)kl;
        ESP_LOGI(TAG, "Config loaded from %s", USB_SETTINGS_PATH);
    } else {
        ESP_LOGW(TAG, "Failed to parse %s, using defaults", USB_SETTINGS_PATH);
    }

    fclose(f);

    /* Apply keyboard layout */
    keycodes_set_layout(s_config.keyboard_layout);
    ESP_LOGI(TAG, "Keyboard layout set: %s", s_config.keyboard_layout == 1 ? "DE" : "US");

    return ESP_OK;
}

deskflow_config_t *config_get_current(void)
{
    return &s_config;
}

/* ------------------------------------------------------------------ */
/* Helper: read a PEM file from USB into a malloc'd buffer            */
/* ------------------------------------------------------------------ */
static int _read_pem(const char *path, unsigned char **buf, unsigned int *len)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) { fclose(f); return -1; }

    unsigned char *b = malloc((size_t)sz + 1);
    if (b == NULL) { fclose(f); return -1; }

    size_t r = fread(b, 1, (size_t)sz, f);
    fclose(f);

    /* Strip carriage returns — mbedtls PEM parser chokes on CRLF */
    size_t w = 0;
    for (size_t i = 0; i < r; i++) {
        if (b[i] != '\r') b[w++] = b[i];
    }
    /* Ensure trailing newline — mbedtls PEM parser requires it */
    if (w == 0 || b[w - 1] != '\n') {
        b[w++] = '\n';
    }
    b[w] = 0;

    *buf = b;
    *len = (unsigned int)(w + 1);  // include null terminator for mbedtls PEM parser
    return 0;
}

/* ------------------------------------------------------------------ */
/* Free all certificate buffers                                       */
/* ------------------------------------------------------------------ */
void config_free_certs(void)
{
    free(s_ca_buf);
    s_ca_buf = NULL;
    s_ca_len = 0;
    free(s_client_cert);
    s_client_cert = NULL;
    s_client_cert_len = 0;
    free(s_client_key);
    s_client_key = NULL;
    s_client_key_len = 0;
}

/* ------------------------------------------------------------------ */
/* Load certs from USB into RAM (call once at boot)                   */
/* ------------------------------------------------------------------ */
esp_err_t config_load_certs(void)
{
    /* Free any previously loaded certs first */
    config_free_certs();

    /* Caller must ensure USB is mounted */
    if (!usb_host_msc_is_mounted()) {
        ESP_LOGI(TAG, "USB not mounted, certs not loaded");
        return ESP_ERR_NOT_FOUND;
    }

    bool any_loaded = false;

    if (_read_pem(USB_CA_PATH, &s_ca_buf, &s_ca_len) == 0) {
        ESP_LOGI(TAG, "Loaded CA cert (%u bytes)", s_ca_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_ca_buf, (s_ca_len > 32 ? 32 : s_ca_len), ESP_LOG_INFO);
        /* Dump tail to verify END marker */
        if (s_ca_len > 32) {
            ESP_LOGI(TAG, "CA tail:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_ca_buf + s_ca_len - 32, 32, ESP_LOG_INFO);
        }
        any_loaded = true;
    }
    if (_read_pem(USB_CLIENT_CERT_PATH, &s_client_cert, &s_client_cert_len) == 0) {
        ESP_LOGI(TAG, "Loaded client cert (%u bytes)", s_client_cert_len);
        any_loaded = true;
    }
    if (_read_pem(USB_CLIENT_KEY_PATH, &s_client_key, &s_client_key_len) == 0) {
        ESP_LOGI(TAG, "Loaded client key (%u bytes)", s_client_key_len);
        any_loaded = true;
    }

    if (!any_loaded) {
        ESP_LOGW(TAG, "No certs loaded from USB");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "All certs loaded");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Upload a single cert to USB and update RAM                         */
/* ------------------------------------------------------------------ */
esp_err_t config_upload_cert(int type, const char *data, size_t len)
{
    const char *path;
    unsigned char **buf_ptr;
    unsigned int *len_ptr;

    switch (type) {
        case 0: path = USB_CA_PATH; buf_ptr = &s_ca_buf; len_ptr = &s_ca_len; break;
        case 1: path = USB_CLIENT_CERT_PATH; buf_ptr = &s_client_cert; len_ptr = &s_client_cert_len; break;
        case 2: path = USB_CLIENT_KEY_PATH; buf_ptr = &s_client_key; len_ptr = &s_client_key_len; break;
        default: return ESP_ERR_INVALID_ARG;
    }

    if (!usb_host_msc_is_mounted()) {
        ESP_LOGW(TAG, "USB not mounted, cannot upload cert");
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return ESP_FAIL;
    }

    fwrite(data, 1, len, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    /* Update RAM buffer — allocate new before freeing old to avoid use-after-free */
    unsigned char *new_buf = malloc(len + 1);
    if (new_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(new_buf, data, len);
    new_buf[len] = 0;
    free(*buf_ptr);
    *buf_ptr = new_buf;
    *len_ptr = (unsigned int)len;

    ESP_LOGI(TAG, "Cert uploaded: %s (%u bytes)", path, *len_ptr);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Accessors                                                          */
/* ------------------------------------------------------------------ */
const unsigned char *config_get_ca(void)              { return s_ca_buf; }
unsigned int         config_get_ca_len(void)          { return s_ca_len; }
const unsigned char *config_get_client_cert(void)     { return s_client_cert; }
unsigned int         config_get_client_cert_len(void) { return s_client_cert_len; }
const unsigned char *config_get_client_key(void)      { return s_client_key; }
unsigned int         config_get_client_key_len(void)  { return s_client_key_len; }
