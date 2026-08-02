#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct deskflow_config {
    char server[64];           // Server hostname or IP
    uint16_t port;             // Server port (default: 24601 for TLS)
    char device_name[32];      // Client screen name
    bool keep_awake;           // Enable jiggle to prevent screensaver
    uint16_t jiggle_interval;  // Seconds between jiggles
    uint16_t screen_width;     // Virtual screen width (physical pixels)
    uint16_t screen_height;    // Virtual screen height (physical pixels)
    uint16_t scaling;          // Display scaling percentage (100=none, 125, 150, 200)
    uint8_t  keyboard_layout;  // Keyboard layout (0=US, 1=DE)
    int16_t  vlan_id;          // VLAN ID: -1 = disabled, 1-4094 = VLAN ID
} deskflow_config_t;

esp_err_t config_load(void);

/** Save config to USB stick at /usb0/settings.json */
esp_err_t config_save(deskflow_config_t *cfg);

/** Get pointer to the currently active config (set by app_main) */
deskflow_config_t *config_get_current(void);

/** Apply network settings (VLAN).  Currently a dummy placeholder. */
void config_apply_network(void);

/** Load certificates from USB into RAM. Call once at boot. */
esp_err_t config_load_certs(void);

/** Return pointer to CA cert buffer (NULL if not loaded) */
const unsigned char *config_get_ca(void);

/** Return length of CA cert buffer */
unsigned int config_get_ca_len(void);

/** Return pointer to client cert buffer (NULL if not loaded) */
const unsigned char *config_get_client_cert(void);

/** Return length of client cert buffer */
unsigned int config_get_client_cert_len(void);

/** Return pointer to client key buffer (NULL if not loaded) */
const unsigned char *config_get_client_key(void);

/** Return length of client key buffer */
unsigned int config_get_client_key_len(void);

/** Upload a cert to USB. type: 0=ca, 1=client_cert, 2=client_key. Returns ESP_OK on success. */
esp_err_t config_upload_cert(int type, const char *data, size_t len);

/** Free all loaded certificate buffers. */
void config_free_certs(void);

#endif
