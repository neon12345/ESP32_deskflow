# ESP32_deskflow — deskflow Client for ESP32-P4

A [deskflow](https://github.com/deskflow/deskflow) client for the ESP32-P4. Connects over **Ethernet** via **TLS 1.3**, speaks the **Barrier protocol**, and presents as a **USB HID device** (keyboard + absolute mouse) through TinyUSB.

## Features

- **Barrier Protocol** — Full deskflow/Barrier protocol client with TLS 1.3 encrypted transport
- **USB HID Device** — Presents as keyboard + absolute mouse to the host PC
- **Multi-Layout Keyboard** — Supports US and German (DE) keyboard layouts with proper AltGr/Right Alt handling
- **Absolute Mouse** — Precise cursor positioning via absolute coordinates (no acceleration drift)
- **Web Configuration** — Built-in HTTP server for runtime configuration without flashing
- **Grove UART Bridge** — UART2 passthrough via web terminal (POST send, WebSocket receive)
- **Status LED** — RGB LED for connection/activity status indication
- **Auto-Reconnect** — Exponential backoff reconnection on network/TLS failures

## Architecture

```
+-----------------------------------------------------------------------+
|                      ESP32-P4 (deskflow client)                        |
|                                                                         |
|  Data Plane (Ethernet -> Server):                                       |
|  +-------------+   +---------------------+   +------------------------+ |
|  | EMAC + PHY  |-> | esp_netif (DHCP)     |-> | esp_tls / mbedTLS     | |
|  | (esp_eth)   |   | + LWIP Stack         |   | (TLS 1.3 client)      | |
|  +-------------+   +---------------------+   +---+--------------------+ |
|                                                               |        |
|  Control Plane (Barrier Protocol -> HID):                      |        |
|  +------------------+   +-------------------+   +--------------+|       |
|  | Barrier Protocol |-> | Actuator Dispatch  |-> | TinyUSB HID | |
|  | Packet Parser    |   | (keyboard, mouse)  |   | Device       | |
|  | (QINF, DMMV, ..) |   +--------------------+   | (tusb_hid)   | |
|  +------------------+                            +--------------+|
|                                                                         |
|  Support:                                                                |
|  +--------------------+   +------------------+   +--------------------+  |
|  | FreeRTOS Tasks     |   | USB Configuration|   | Reconnect (backoff)| |
|  | (barrier, tusb,    |   | (server, name,   |   +--------------------+ |
|  |  network, config)  |   |  jiggle, layout) |                            |
|  +--------------------+   +------------------+                            |
+-----------------------------------------------------------------------+
```

## Source Projects

| Component | Source | Role |
|-----------|--------|------|
| Ethernet Stack | ESP-IDF esp-p4-eth example | EMAC init, PHY config, DHCP, esp_netif |
| Barrier/deskflow Protocol | [esparrier](https://github.com/windoze/esparrier) (Rust) | Packet format, state machine, actuator interface |
| TLS 1.3 | ESP-IDF mbedTLS (esp_tls) | Secure transport over TCP |
| USB HID Device | ESP-IDF tusb_hid example | TinyUSB HID device (keyboard + absolute mouse) |

### Upstream Projects

- [**deskflow**](https://github.com/deskflow/deskflow) — Fork of Barrier, the screen sharing software this client connects to
- [**Barrier**](https://github.com/debauchee/barrier) — Original open-source KVM over network (upstream of deskflow)
- [**esparrier**](https://github.com/windoze/esparrier) — Rust Barrier client that provided the protocol reference implementation

## Project Structure

```
ESP32_deskflow/
├── CMakeLists.txt                # Root project (C11)
├── sdkconfig.defaults            # ESP32-P4 defaults (Ethernet, TLS, TinyUSB)
├── partitions.csv                # OTA, PHY, cert partitions
├── main/
│   ├── CMakeLists.txt           # Component registration
│   ├── Kconfig.projbuild        # Ethernet GPIO/menuconfig options
│   ├── idf_component.yml        # esp_tinyusb dependency
│   ├── main.c                   # App entry: USB MSC, Ethernet, TinyUSB, Barrier client
│   ├── button_reset.h/.c        # Reset button (GPIO 45)
│   ├── barrier/
│   │   ├── barrier_io.h/.c      # Big-endian binary I/O helpers
│   │   ├── barrier_packet.h/.c  # Packet parse/serialize (23 Barrier types)
│   │   └── barrier_client.h/.c  # TLS handshake, packet dispatch, reconnect loop
│   ├── network/
│   │   ├── eth_network.h/.c     # EMAC+PHY init, event-driven link/IP tracking
│   │   ├── network_events.h     # Ethernet event group bit definitions
│   │   ├── tls_client.h/.c      # TLS 1.3 client wrapper (esp_tls_conn_new_sync)
│   │   └── reconnect.h/.c       # Exponential backoff (1s base, 60s max, jitter)
│   ├── tusb/
│   │   ├── tusb_device.h/.c     # TinyUSB driver init + HID descriptors + callbacks
│   │   ├── tusb_actuator.h/.c   # Barrier-to-HID translator (keyboard, absolute mouse, jiggle)
│   │   └── keycodes.h/.c        # Barrier-to-HID keycode lookup (US + German layouts)
│   ├── config/
│   │   └── config.h/.c          # USB config (server, port, name, jiggle, screen size, layout)
│   ├── web/
│   │   ├── web_server.h/.c      # HTTP settings server + UART WebSocket terminal
│   │   └── uart_page.h          # UART terminal HTML page
│   ├── grove/
│   │   └── grove_uart.h/.c      # UART2 driver (G54 TX / G53 RX, 115200 8N1)
│   ├── led/
│   │   └── led_rgb.h/.c         # RGB status LED driver
│   └── usb_host/
│       └── usb_host_msc.h/.c    # USB host MSC support
```

## Quick Start

### Prerequisites

- ESP-IDF v6.0.2 (with `esp_tinyusb` component registry access)
- ESP32-P4 dev board with Ethernet (RMII)
- USB connection to host PC (device mode)
- **FAT32-formatted USB stick** with TLS certificates (`server_public_cert.pem`, optionally `client_public_cert.pem` + `client_private_key.pem` for mTLS)

### Build and Flash

```bash
cd ESP32_deskflow
idf.py set-target esp32p4
idf.py menuconfig          # Configure server address, Ethernet GPIOs, etc.
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Configure Server Address

The server address and other settings are configured via the built-in web interface:

1. Flash and connect the ESP32-P4 to your network
2. Open `http://<esp32-ip>/` in a browser
3. Enter the deskflow server address, port, and other settings
4. Click Save

### Default Configuration

| Parameter | Default | Description |
|-----------|---------|-------------|
| Server | `192.168.1.10` | deskflow server IP or hostname |
| Port | `24601` | TLS port |
| Device name | `deskflow-client` | Screen name sent to server |
| Screen size | `1920x1080` | Virtual screen dimensions |
| Scaling | `100` | Display scaling percentage |
| Keyboard layout | `US` | US or German (DE) |
| Jiggle interval | `30` seconds | Keep-alive mouse movement interval |
| Keep awake | `false` | Enable jiggle to prevent screensaver |

## Ethernet Configuration

Use `idf.py menuconfig` -> **deskflow Ethernet Configuration** to configure:

- PHY interface (default or RMII)
- RMII clock mode (input from external or output from internal)
- RMII GPIO assignments (ESP32-P4 defaults pre-configured)
- SMI MDC/MDIO GPIOs
- PHY address and reset GPIO

## Grove UART

The Grove connector provides UART2 passthrough:

| Grove Pin | ESP32-P4 Pin | Color | Function |
|-----------|-------------|-------|----------|
| TX | G54 | Yellow | UART2 TX |
| RX | G53 | White | UART2 RX |

**Settings:** 115200 baud, 8N1, no flow control.

**Web Terminal:** Access the UART terminal at `http://<esp32-ip>/uart`:
- **Send:** POST to `/api/uart/write` (or use the web terminal send button)
- **Receive:** WebSocket at `/api/uart/ws` (UART data pushed to browser)

## USB Host MSC

The ESP32-P4 USB host port (pins 50/49, HS PHY) supports mass storage devices. Plug in a FAT32-formatted USB stick and it mounts at `/usb0`. Configuration and TLS certificates are loaded from the drive.

### USB Stick Contents

| File | Purpose |
|------|---------|
| `settings.json` | Runtime configuration (server, port, name, screen size, scaling, layout, jiggle, keep_awake) |
| `server_public_cert.pem` | Server public certificate for TLS verification |
| `client_public_cert.pem` | Client certificate for mutual TLS (mTLS) |
| `client_private_key.pem` | Client private key for mutual TLS (mTLS) |

The config is automatically reloaded when a USB stick is inserted. Certificates are loaded into RAM at boot.

## Web Interface

The built-in HTTP server provides two pages for configuration and debugging:

### Settings Page (`http://<esp32-ip>/`)

Configures the deskflow client without reflashing:

- Server address and port
- Device name
- Screen resolution and display scaling
- Keyboard layout (US / German DE)
- Keep-alive jiggle interval
- Keep awake toggle
- Certificate upload (CA, client cert, client key)
- Reboot button

Settings are saved to `settings.json` on the USB stick.

### UART Terminal (`http://<esp32-ip>/uart`)

Web-based terminal for the Grove UART bridge:

- **Receive:** UART data pushed to the browser in real-time via WebSocket
- **Send:** Text sent to UART via POST (or POST read to poll)
- Useful for debugging serial devices connected to the Grove connector

## Expected Console Output

```
I (xxx) deskflow_main: deskflow client starting
I (xxx) deskflow_main: Config: server=192.168.1.10:24601 name=deskflow-client screen=1920x1080 jiggle=30s keep_awake=0
I (xxx) eth_network: Ethernet Started
I (xxx) eth_network: Ethernet Link Up
I (xxx) eth_network: Ethernet HW Addr XX:XX:XX:XX:XX:XX
I (xxx) eth_network: Ethernet Got IP Address
I (xxx) eth_network: ETHIP:192.168.1.XX
I (xxx) tusb_device: TinyUSB device initialized
I (xxx) tusb_actuator: Actuator initialized (screen: 1920x1080)
I (xxx) barrier_client: Barrier client created
I (xxx) barrier_client: Barrier client task started
I (xxx) deskflow_main: deskflow client running
I (xxx) tls_client: TLS 1.3 connection established to 192.168.1.10:24601
I (xxx) barrier_client: Connected to Barrier server
```

## How It Works

1. **Startup**: Config loaded from USB stick, Ethernet and TinyUSB are started
2. **Barrier client task**: Creates TLS connection, performs Barrier protocol handshake, enters main packet dispatch loop
3. **Packet dispatch**: Server commands (mouse move, key events, cursor enter/leave) are translated into TinyUSB HID reports
4. **Reconnection**: On TLS or network failure, exponential backoff reconnection (1s base, 60s max, with jitter)

## Memory Budget

| Component | Estimate |
|-----------|----------|
| FreeRTOS kernel | ~16 KB |
| LWIP + esp_netif | ~40 KB |
| mbedTLS (TLS 1.3) | ~30 KB |
| TinyUSB HID | ~8 KB |
| Barrier protocol | ~2 KB |
| Task stacks | ~18 KB |
| **Total minimum** | **~114 KB** |

ESP32-P4 has 8 MB SRAM + 16 MB PSRAM -- memory is not a constraint.

## Security

- TLS 1.3 is enforced for all client-server communication
- CA certificate verification is supported (load `ca_cert.pem` onto USB stick)
- Hostname verification enabled by default
- Production builds should set `CONFIG_MBEDTLS_TLS_CLIENT_INSECURE_MODE=n`

## Limitations

### Keyboard Layouts

Only **US** and **DE** (German) keyboard layouts are available. The German keymap is **incomplete** — some characters may not map correctly.

## Debugging

Monitor the serial console for tagged log output:

| Tag | Component |
|-----|-----------|
| `deskflow_main` | Application startup and monitoring |
| `barrier_client` | Barrier protocol state machine and packet dispatch |
| `tls_client` | TLS 1.3 connection management |
| `eth_network` | Ethernet link state and IP acquisition |
| `tusb_device` | TinyUSB driver and HID callbacks |
| `tusb_actuator` | HID report generation |
| `web_server` | HTTP settings server and UART terminal |
| `grove_uart` | UART2 driver |

## License

This project integrates code adapted from:
- ESP-IDF examples (Apache-2.0 / CC0-1.0)
- esparrier (MIT License)
- deskflow / Barrier protocol specification

Licensed under MIT — see [LICENSE](LICENSE).
