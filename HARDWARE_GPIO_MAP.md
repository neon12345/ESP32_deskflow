# M5Stack Unit PoE-P4 GPIO Map

ESP32_deskflow targets the **M5Stack Unit PoE-P4** board, which is built around the ESP32-P4 SoC.

## Board Connectors & Signals

### Ethernet (RMII + SMI)

| ESP32-P4 | Signal | Used in code? |
|----------|--------|---------------|
| G28 | RMII_CRS_DV | ✅ `CONFIG_ETH_RMII_CRS_DV_GPIO=28` |
| G29 | RMII_RXD0 | ✅ `CONFIG_ETH_RMII_RXD0_GPIO=29` |
| G30 | RMII_RXD1 | ✅ `CONFIG_ETH_RMII_RXD1_GPIO=30` |
| G31 | SMI_MDC | ✅ `CONFIG_ETH_MDC_GPIO=31` |
| G32 | RMII_CLK loopback | ✅ `CONFIG_ETH_RMII_CLK_EXT_LOOPBACK_IN_GPIO=32` |
| G34 | RMII_TXD0 | ✅ `CONFIG_ETH_RMII_TXD0_GPIO=34` |
| G35 | RMII_TXD1 | ✅ `CONFIG_ETH_RMII_TXD1_GPIO=35` |
| G49 | RMII_TX_EN | ✅ `CONFIG_ETH_RMII_TX_EN_GPIO=49` |
| G50 | RMII_CLK | ✅ `CONFIG_ETH_RMII_CLK_GPIO=50` |
| G51 | PHY_RST | ✅ `CONFIG_ETH_PHY_RST_GPIO=51` |
| G52 | SMI_MDIO | ✅ `CONFIG_ETH_MDIO_GPIO=52` |

### LCD (MIPI DSI)

| ESP32-P4 | Signal | Used in code? |
|----------|--------|---------------|
| G4 | LCD_RST | ❌ (board hardware) |
| G33 | LCD_BL | ❌ (board hardware) |
| DSI_CLK_N/P, DSI_D0_N/P, DSI_D1_N/P | MIPI DSI lanes | Hardwired, not GPIO |

### Touch Panel (I2C)

| ESP32-P4 | Signal | Used in code? |
|----------|--------|---------------|
| G0 | TP_SDA (shared w/ camera) | ❌ (board hardware) |
| G1 | TP_SCL (shared w/ camera) | ❌ (board hardware) |
| G2 | TP_RST | ❌ (board hardware) |
| G3 | TP_INT | ❌ (board hardware) |

### Camera

| ESP32-P4 | Signal | Used in code? |
|----------|--------|---------------|
| G0 | CAM_SDA (shared w/ touch) | ❌ (board hardware) |
| G1 | CAM_SCL (shared w/ touch) | ❌ (board hardware) |
| G18 | CAM_RST | ❌ (board hardware) |
| G36 | CAM_MCLK | ❌ (board hardware) |
| CSI_CLK_N/P, CSI_D0_N/P, CSI_D1_N/P | MIPI CSI lanes | Hardwired, not GPIO |

### USB

| ESP32-P4 | Signal | Used in code? |
|----------|--------|---------------|
| USB2_OTG_DP/DN | USB 2.0 HS HOST | Hardwired PHY |
| G24/G25 | USB0_DP/DN (Download/OTG) | ✅ **TinyUSB HID device** |
| G26/G27 | USB1_DP/DN (ISP connector) | Hardwired |

### IR / RGB LED / Button

| ESP32-P4 | Signal | Used in code? |
|----------|--------|---------------|
| G14 | IR_TX | ❌ **FREE** |
| G15 | LED_GREEN | ❌ **FREE** |
| G16 | LED_BLUE | ❌ **FREE** |
| G17 | LED_RED | ❌ **FREE** |
| G45 | KEY_USER | ❌ **FREE** |

### HY2.0-4P (Grove)

| Wire | ESP32-P4 | Used in code? |
|------|----------|---------------|
| Black | GND | — |
| Red | 5V | — |
| Yellow | G53 | ✅ `grove_bridge.c` UART2 TX |
| White | G54 | ✅ `grove_bridge.c` UART2 RX |

### ISP / SDIO Connector

| Pin | Left (GPIO) | Right (GPIO) | Notes | Used in code? |
|-----|------------|-------------|-------|---------------|
| 1 | G13 | GND | | ❌ **FREE** |
| 2 | G12 | G35 (BOOT) | G35 = ETH TXD1 | G12: ❌ **FREE** |
| 3 | G11 | RST | | ❌ **FREE** |
| 4 | G10 | G38 (UART0_RX) | Board silkscreen | ❌ **FREE** |
| 5 | G9 | G37 (UART0_TX) | Board silkscreen | ❌ **FREE** |
| 6 | G8 | 3V3 | | ❌ **FREE** |
| 7 | G27 (USB1_DP) | — | Hardwired | — |
| 8 | G26 (USB1_DM) | — | Hardwired | — |
| 9 | 5V | — | | — |

### Hat2-Bus

| Pin | Left (GPIO) | Right (GPIO) | Used in code? |
|-----|-------------|-------------|---------------|
| 1 | GND | G44 | G44: ❌ **FREE** |
| 3 | 5V | G43 | G43: ❌ **FREE** |
| 5 | G21 | G42 | ❌ **FREE** |
| 7 | G20 | G41 | ❌ **FREE** |
| 9 | G19 | G40 | ❌ **FREE** |
| 11 | NC | G39 | G39: ❌ **FREE** |
| 13 | 3V3 | G23 | G23: ❌ **FREE** |
| 15 | 5V | G22 | G22: ❌ **FREE** |

---

## GPIO Usage Audit (ESP32_deskflow code)

Scanned all `.c` and `.h` files in `ESP32_deskflow/main/`. **Zero hardcoded GPIO constants**. All GPIO assignments come from `sdkconfig.defaults` Kconfig macros.

### Only used GPIOs (via `sdkconfig.defaults`)

All GPIO assignments come from Kconfig macros in `eth_network.c`. **Zero hardcoded GPIO constants** in any `.c` or `.h` file.

| GPIO | Function | Source |
|------|----------|--------|
| 28 | ETH RMII CRS_DV | `CONFIG_ETH_RMII_CRS_DV_GPIO` |
| 29 | ETH RMII RXD0 | `CONFIG_ETH_RMII_RXD0_GPIO` |
| 30 | ETH RMII RXD1 | `CONFIG_ETH_RMII_RXD1_GPIO` |
| 31 | ETH MDC | `CONFIG_ETH_MDC_GPIO` |
| 32 | RMII clock loopback | `CONFIG_ETH_RMII_CLK_EXT_LOOPBACK_IN_GPIO` |
| 34 | ETH RMII TXD0 | `CONFIG_ETH_RMII_TXD0_GPIO` |
| 35 | ETH RMII TXD1 | `CONFIG_ETH_RMII_TXD1_GPIO` |
| 49 | ETH RMII TX_EN | `CONFIG_ETH_RMII_TX_EN_GPIO` |
| 50 | ETH RMII CLK | `CONFIG_ETH_RMII_CLK_GPIO` |
| 51 | ETH PHY reset | `CONFIG_ETH_PHY_RST_GPIO` |
| 52 | ETH MDIO | `CONFIG_ETH_MDIO_GPIO` |
| 22 | UART1 TX (debug console) | `CONFIG_ESP_CONSOLE_UART_TX_NUM=22` |
| 23 | UART1 RX (debug console) | `CONFIG_ESP_CONSOLE_UART_RX_NUM=23` |
| 53 | UART2 TX (Grove bridge) | `grove_bridge.c` |
| 54 | UART2 RX (Grove bridge) | `grove_bridge.c` |

### Hardwired (not GPIO-matrix routable)

| Pin | Signal | Notes |
|-----|--------|-------|
| USB0_DP (G24), USB0_DN (G25) | USB 2.0 FS | **TinyUSB HID** |
| USB1_DP (G27), USB1_DM (G26) | USB 2.0 FS | ISP connector |
| USB2_OTG_DP/DN | USB 2.0 HS | Hardwired PHY |
| DSI_CLK_N/P, DSI_D0_N/P, DSI_D1_N/P | MIPI DSI | LCD hardwired |
| CSI_CLK_N/P, CSI_D0_N/P, CSI_D1_N/P | MIPI CSI | Camera hardwired |

### All other GPIOs are free

**22 GPIOs available** for future use:

| GPIO | Connector | Good for |
|------|-----------|----------|
| G8 | ISP pin 6 | UART TX/RX, GPIO |
| G9 | ISP pin 5 (UART0_TX silkscreen) | **Debug console TX** |
| G10 | ISP pin 4 | GPIO, UART |
| G11 | ISP pin 3 | GPIO, UART |
| G12 | ISP pin 2 | GPIO, UART |
| G13 | ISP pin 1 | GPIO, UART |
| G14 | IR header | IR, GPIO |
| G15 | RGB LED (Green) | **Debug LED** |
| G16 | RGB LED (Blue) | **Debug LED** |
| G17 | RGB LED (Red) | **Debug LED** |
| G19 | Hat2-Bus pin 9 | **Serial device UART TX/RX** |
| G20 | Hat2-Bus pin 7 | Serial device UART TX/RX |
| G21 | Hat2-Bus pin 5 | Serial device UART TX/RX |
| G22 | Hat2-Bus pin 16 | GPIO |
| G23 | Hat2-Bus pin 14 | GPIO |
| G33 | LCD header | PWM (backlight) |
| G37 | ISP (UART0_TX silkscreen) | **Debug console TX** |
| G38 | ISP (UART0_RX silkscreen) | **Debug console RX** |
| G39 | Hat2-Bus pin 12 | GPIO |
| G40 | Hat2-Bus pin 10 | GPIO |
| G41 | Hat2-Bus pin 8 | GPIO |
| G42 | Hat2-Bus pin 6 | GPIO |
| G43 | Hat2-Bus pin 4 | GPIO |
| G44 | Hat2-Bus pin 2 | GPIO |
| G45 | Button header | Button input |

> **Note:** G0, G1, G2, G3, G18, G36 are marked "not used by code" but are wired to board components (touch, camera). Avoid reassigning them.

---

## Recommended GPIO Allocation for deskflow

| Purpose | GPIO | Connector | Why |
|---------|------|-----------|-----|
| ESP_LOG console (UART0 TX) | G37 | ISP pin 5 | Board silkscreened, easy USB-to-TTL |
| ESP_LOG console (UART0 RX) | G38 | ISP pin 4 | Board silkscreened, easy USB-to-TTL |
| External serial (UART1 TX) | G19 | Hat2-Bus pin 9 | Adjacent to G20/G21, hat connector is plug-and-play |
| External serial (UART1 RX) | G21 | Hat2-Bus pin 5 | Adjacent to G19/G20 |
| Debug LED (status) | G15 (Green) | Onboard | Visible, easy to wire |
| Debug LED (error) | G17 (Red) | Onboard | Visible, easy to wire |
| User button | G45 | Onboard | Already silkscreened as KEY_USER |

---

## UART Resource Summary

ESP32-P4 has **4 UART peripherals** (UART0–UART3). All pins are flexible via GPIO matrix.

| UART | Recommended TX/RX | Connector | Purpose |
|------|-------------------|-----------|---------|
| UART0 | G37 / G38 | ISP | ESP_LOG debug console |
| UART1 | G19 / G21 | Hat2-Bus | External serial device |
| UART2 | G53 / G54 | HY2.0-4P | **Grove TCP bridge (:8765)** |
| UART3 | G10 / G11 | ISP | Spare |
