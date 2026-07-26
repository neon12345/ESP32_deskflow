/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "eth_network.h"
#include "network_events.h"

static const char *TAG = "eth_network";

#define MAC_ADDR_LEN  6

static eth_state_t *s_eth_state = NULL;
static EventGroupHandle_t s_event_group = NULL;

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data);
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data);

/**
 * @brief Initialize Ethernet driver with generic PHY (all IEEE 802.3 compliant PHYs)
 *
 * @param[out] state Ethernet state structure to populate
 * @return
 *          - ESP_OK on success
 *          - ESP_ERR_INVALID_ARG when passed invalid pointer
 *          - ESP_FAIL on any other failure
 */
esp_err_t eth_init(eth_state_t *state, EventGroupHandle_t event_group)
{
    if (state == NULL) {
        ESP_LOGE(TAG, "invalid argument: state cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(eth_state_t));
    s_eth_state = state;
    s_event_group = event_group;

    /* Initialize TCP/IP network interface (esp-netif) */
    ESP_ERROR_CHECK(esp_netif_init());
    /* Create default event loop that runs in background */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create instance of esp-netif for Ethernet */
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    state->eth_netif = esp_netif_new(&cfg);
    if (state->eth_netif == NULL) {
        ESP_LOGE(TAG, "create ethernet netif failed");
        return ESP_FAIL;
    }

    /* Init common MAC and PHY configs to default */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    /* Update PHY config based on board specific configuration */
    phy_config.phy_addr = CONFIG_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_ETH_PHY_RST_GPIO;
#if CONFIG_ETH_PHY_RST_TIMING_EN
    phy_config.hw_reset_assert_time_us = CONFIG_ETH_PHY_RST_ASSERT_TIME_US;
    phy_config.post_hw_reset_delay_ms = CONFIG_ETH_PHY_RST_DELAY_MS;
#endif /* CONFIG_ETH_PHY_RST_TIMING_EN */

    /* Init vendor specific MAC config to default */
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    /* Update vendor specific MAC config based on board configuration */
    esp32_emac_config.smi_gpio.mdc_num = CONFIG_ETH_MDC_GPIO;
    esp32_emac_config.smi_gpio.mdio_num = CONFIG_ETH_MDIO_GPIO;

#if CONFIG_ETH_PHY_INTERFACE_RMII
    /* Configure RMII based on Kconfig when non-default configuration selected */
    esp32_emac_config.interface = EMAC_DATA_INTERFACE_RMII;

    /* Configure RMII clock mode and GPIO */
#if CONFIG_ETH_RMII_CLK_INPUT
    esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
#else /* CONFIG_ETH_RMII_CLK_OUTPUT */
    esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
#endif
    esp32_emac_config.clock_config.rmii.clock_gpio = CONFIG_ETH_RMII_CLK_GPIO;

#if CONFIG_ETH_RMII_CLK_EXT_LOOPBACK_EN
    esp32_emac_config.clock_config_out_in.rmii.clock_gpio = CONFIG_ETH_RMII_CLK_EXT_LOOPBACK_IN_GPIO;
    esp32_emac_config.clock_config_out_in.rmii.clock_mode = EMAC_CLK_EXT_IN;
#endif

#if SOC_EMAC_USE_MULTI_IO_MUX
    /* Configure RMII datapath GPIOs */
    esp32_emac_config.emac_dataif_gpio.rmii.tx_en_num = CONFIG_ETH_RMII_TX_EN_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.txd0_num = CONFIG_ETH_RMII_TXD0_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.txd1_num = CONFIG_ETH_RMII_TXD1_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.crs_dv_num = CONFIG_ETH_RMII_CRS_DV_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.rxd0_num = CONFIG_ETH_RMII_RXD0_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.rxd1_num = CONFIG_ETH_RMII_RXD1_GPIO;
#endif /* SOC_EMAC_USE_MULTI_IO_MUX */
#endif /* CONFIG_ETH_PHY_INTERFACE_RMII */

    /* Create new ESP32 Ethernet MAC instance */
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "create MAC instance failed");
        esp_netif_destroy(state->eth_netif);
        return ESP_FAIL;
    }

    /* Create new generic PHY instance */
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "create PHY instance failed");
        mac->del(mac);
        esp_netif_destroy(state->eth_netif);
        return ESP_FAIL;
    }

    /* Init Ethernet driver to default and install it */
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&eth_config, &state->eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed");
        mac->del(mac);
        phy->del(phy);
        esp_netif_destroy(state->eth_netif);
        return ESP_FAIL;
    }

    /* Create glue and attach Ethernet driver to TCP/IP stack */
    state->glue = esp_eth_new_netif_glue(state->eth_handle);
    if (state->glue == NULL) {
        ESP_LOGE(TAG, "create netif glue failed");
        esp_eth_driver_uninstall(state->eth_handle);
        mac->del(mac);
        phy->del(phy);
        esp_netif_destroy(state->eth_netif);
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_netif_attach(state->eth_netif, state->glue));

    esp_err_t ret;

    /* Register user defined event handlers */
    if ((ret = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                          &eth_event_handler, NULL)) != ESP_OK) {
        ESP_LOGE(TAG, "register ETH event handler failed");
        esp_eth_del_netif_glue(state->glue);
        esp_eth_driver_uninstall(state->eth_handle);
        mac->del(mac);
        phy->del(phy);
        esp_netif_destroy(state->eth_netif);
        return ret;
    }
    if ((ret = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                          &got_ip_event_handler, NULL)) != ESP_OK) {
        ESP_LOGE(TAG, "register IP event handler failed");
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
        esp_eth_del_netif_glue(state->glue);
        esp_eth_driver_uninstall(state->eth_handle);
        mac->del(mac);
        phy->del(phy);
        esp_netif_destroy(state->eth_netif);
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief Start Ethernet driver state machine
 */
esp_err_t eth_start(eth_state_t *state)
{
    if (state == NULL || state->eth_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_eth_start(state->eth_handle);
}

/**
 * @brief Stop Ethernet driver and clean up resources
 */
void eth_stop(eth_state_t *state)
{
    if (state == NULL) {
        return;
    }

    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler);

    if (state->eth_handle != NULL) {
        esp_eth_stop(state->eth_handle);
        esp_eth_del_netif_glue(state->glue);
        esp_eth_driver_uninstall(state->eth_handle);

        esp_eth_mac_t *mac = NULL;
        esp_eth_phy_t *phy = NULL;
        esp_eth_get_mac_instance(state->eth_handle, &mac);
        esp_eth_get_phy_instance(state->eth_handle, &phy);
        if (mac != NULL) {
            mac->del(mac);
        }
        if (phy != NULL) {
            phy->del(phy);
        }

        state->eth_handle = NULL;
        state->glue = NULL;
    }

    if (state->eth_netif != NULL) {
        esp_netif_destroy(state->eth_netif);
        state->eth_netif = NULL;
    }

    state->link_up = false;
    state->has_ip = false;
    s_eth_state = NULL;
    s_event_group = NULL;
}

/**
 * @brief Event handler for Ethernet events
 */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    uint8_t mac_addr[MAC_ADDR_LEN] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
        if (s_eth_state != NULL) {
            s_eth_state->link_up = true;
        }
        if (s_event_group != NULL) {
            xEventGroupSetBits(s_event_group, EVENT_ETH_LINK_UP);
            xEventGroupClearBits(s_event_group, EVENT_ETH_LINK_DOWN);
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        if (s_eth_state != NULL) {
            s_eth_state->link_up = false;
        }
        if (s_event_group != NULL) {
            xEventGroupClearBits(s_event_group, EVENT_ETH_LINK_UP);
            xEventGroupSetBits(s_event_group, EVENT_ETH_LINK_DOWN);
        }
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

/**
 * @brief Event handler for IP_EVENT_ETH_GOT_IP
 */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");

    if (s_eth_state != NULL) {
        s_eth_state->has_ip = true;
    }
    if (s_event_group != NULL) {
        xEventGroupSetBits(s_event_group, EVENT_ETH_HAS_IP);
    }
}
