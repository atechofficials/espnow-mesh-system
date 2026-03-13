# Gateway v1 — Build & Flash Guide

Firmware version: **1.8.0**
Target board: `esp32-s3-devkitc1-n8r8`

---

## Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) or the PlatformIO IDE extension for VS Code
- USB cable connected to the ESP32-S3-DevKitC-1-N8R8

---

## Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

| Library | Version |
|---------|---------|
| WiFiManager | 2.0.17 |
| ESPAsyncWebServer | 3.6.0 |
| AsyncTCP | 3.3.2 |
| ArduinoJson | 7.4.3 |
| Adafruit NeoPixel | 1.15.4 |

Framework libraries used directly (no extra install needed): `LittleFS`, `Preferences`, `WiFi`, `esp_now`.

---

## First-Time Flash

Run these commands in order from the `gateway_v1/` directory:

```bash
# 1. Erase the entire flash (recommended for a clean first install)
pio run --target erase

# 2. Upload the web dashboard assets to LittleFS
pio run --target uploadfs

# 3. Compile and upload the firmware
pio run --target upload
```

For subsequent firmware-only updates (no dashboard changes) you only need step 3.
For dashboard-only updates (HTML/JS/CSS changes) you only need step 2.

---

## First Boot

1. The gateway opens a Wi-Fi access point: **`ESP32-Mesh-Setup`** / password **`meshsetup`**
2. Connect to it from any phone or laptop — a captive portal opens automatically
3. Select your home Wi-Fi network and enter the password
4. The gateway connects, prints its IP address to the serial monitor, and the dashboard becomes available at `http://<ip>/`

Credentials are saved to NVS. On every subsequent boot the gateway reconnects automatically.

---

## Updating the AP Credentials

The captive-portal SSID and password used for initial setup can be changed from the dashboard without reflashing:

1. Open the dashboard → **Gateway Settings** card
2. Enter the new SSID and/or password → **Save**
3. The gateway restores the new values on every subsequent boot

---

## Factory Reset

Hold the **BOOT button (GPIO 0)** for **5 seconds** while the gateway is running. This clears all NVS data (Wi-Fi credentials, AP config, paired nodes) and reboots into captive-portal mode.

---

## Key Configuration (`src/main.cpp`)

All tuneable constants are at the top of `main.cpp`:

| Constant | Default | Description |
|----------|---------|-------------|
| `AP_SSID_DEFAULT` | `"ESP32-Mesh-Setup"` | Captive-portal SSID (first boot) |
| `AP_PASS_DEFAULT` | `"meshsetup"` | Captive-portal password |
| `WEB_PORT` | `80` | HTTP/WebSocket port |
| `WS_UPDATE_MS` | `2000` | How often the gateway pushes node updates to the browser (ms) |
| `WS_META_MS` | `10000` | How often the gateway pushes gateway metadata to the browser (ms) |
| `NODE_TIMEOUT_MS` | `35000` | Time without a message before a node is marked offline (ms) |
| `RX_QUEUE_SIZE` | `30` | Incoming ESP-NOW packet queue depth |

Timing constants shared with the nodes live in `include/mesh_protocol.h`.

---

## Partition Table

The project uses a custom 8 MB partition table (`partitions_8mb.csv`) that allocates a large LittleFS partition for the web assets:

| Partition | Size |
|-----------|------|
| app0 (firmware) | 1.5 MB |
| app1 (OTA slot) | 1.5 MB |
| LittleFS | ~4 MB |
| NVS | 24 KB |

---

## Serial Monitor

```bash
pio device monitor
```

Default baud rate: **115200**. Log prefixes:

| Prefix | Meaning |
|--------|---------|
| `[BOOT]` | Startup sequence |
| `[FS]` | LittleFS mount / file listing |
| `[WiFi]` | Wi-Fi connection events |
| `[ESP-NOW]` | ESP-NOW init and TX callbacks |
| `[DISC]` | Node discovery (beacon detected) |
| `[PAIR]` | Pairing handshake steps |
| `[MESH]` | Node registration (new or restored from NVS) |
| `[SENS]` | Sensor schema registration and incoming readings |
| `[CFG]` | Node settings get / set |
| `[WS]` | WebSocket client connect / disconnect |

---

## WebSocket Message Reference

The dashboard communicates with the gateway over a single WebSocket at `ws://<ip>/ws`.

### Gateway → Browser

| `type` | Description |
|--------|-------------|
| `update` | Full node list with sensor readings (as `sensor_readings[]` id/value array), schema-ready flag, online status, uptime |
| `meta` | Gateway metadata (firmware version, uptime, IP, AP SSID) |
| `discovered` | Nodes broadcasting in pairing mode |
| `pair_timeout` | Pairing window expired |
| `node_settings` | Settings schema + current values for one node |
| `node_sensor_schema` | Sensor schema (labels, units, precision) for one node |
| `ap_config_ack` | Confirmation that new AP config was saved |
| `gw_portal_starting` | Gateway is about to open the captive portal |
| `gw_factory_reset` | Factory reset acknowledged |

### Browser → Gateway

| `type` | Description |
|--------|-------------|
| `relay_cmd` | Toggle a relay on an actuator node |
| `pair_cmd` | Initiate pairing with a discovered node |
| `unpair_cmd` | Disconnect and forget a paired node |
| `rename_node` | Change a node's display name |
| `reboot_node` | Send a reboot command to a specific node |
| `reboot_gw` | Reboot the gateway |
| `set_ap_config` | Update the captive-portal SSID/password |
| `start_wifi_portal` | Re-open the Wi-Fi captive portal |
| `factory_reset` | Erase NVS and reboot |
| `node_settings_get` | Request settings schema for a node |
| `node_settings_set` | Update one setting value on a node |
| `node_sensor_schema_get` | Request sensor schema for a node (served from cache or fetched live) |