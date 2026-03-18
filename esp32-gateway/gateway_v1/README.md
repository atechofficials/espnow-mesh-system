# Gateway v1 - Build, Flash, and OTA Guide

Firmware version: **1.9.0**  
Target board: `esp32-s3-devkitc1-n8r8`

---

## Highlights in v1.9.0

- Added **web-based Gateway OTA firmware updates**
- Added OTA image validation and automatic reboot after successful flash
- Added upload progress and OTA status feedback in the web interface
- Switched the default build to an **OTA-capable 8 MB partition layout**
- Continued support for gateway web credentials, relay label assignment, gateway LED control, and schema-driven node settings

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

Framework libraries used directly (no extra install needed): `LittleFS`, `Preferences`, `WiFi`, `esp_now`, `Update`.

---

## Partition Layout

The project now builds against **`partitions_8mb_ota.csv`**.

| Partition | Size |
|-----------|------|
| `nvs` | 20 KB |
| `otadata` | 8 KB |
| `app0` | 1792 KB |
| `app1` | 1792 KB |
| `LittleFS` | 4544 KB |
| `coredump` | 64 KB |

This layout is required for the new web-based Gateway OTA update feature.

---

## First-Time Flash / Migration to v1.9.0

If you are coming from an older gateway build that used the non-OTA partition table, the **first upgrade to v1.9.0 must be done over USB**.

Run these commands from the `gateway_v1/` directory:

```bash
# 1. Erase flash for a clean migration
pio run --target erase

# 2. Upload the LittleFS web assets
pio run --target uploadfs

# 3. Compile and upload the firmware
pio run --target upload
```

After this migration, future **gateway firmware** updates can be done from the web interface.

If you change only the dashboard assets later, run:

```bash
pio run --target uploadfs
```

If you change only the firmware later and want to use USB instead of OTA, run:

```bash
pio run --target upload
```

---

## First Boot

1. The gateway tries to reconnect to saved Wi-Fi credentials automatically
2. If no router credentials are stored, WiFiManager opens the setup portal AP:
   - SSID: **`ESP32-Mesh-Gateway`**
   - Password: **`meshsetup`**
3. Connect to that AP from a phone or laptop
4. Open the captive portal and select your home Wi-Fi network
5. After connection, the dashboard becomes available at:

```text
http://<gateway-ip>/
```

Wi-Fi credentials, gateway settings, and paired-node records are stored in NVS and survive reboot.

---

## Gateway OTA Update from the Web Interface

Once v1.9.0 is already installed, the gateway can update its **own firmware** directly from the dashboard.

### Steps

1. Build a new gateway firmware binary:

```bash
pio run
```

2. Open the dashboard in a browser
3. Go to **Settings -> Gateway Firmware Update**
4. Select the new `firmware.bin`
5. Click **Upload Gateway Firmware**
6. Confirm the update when prompted

### What the gateway does

- validates the uploaded image
- checks OTA slot size and image type
- flashes the inactive OTA slot
- reports progress/errors in the web UI
- reboots into the new firmware automatically on success

### Notes

- Upload **`firmware.bin`**, not `bootloader.bin`, not `partitions.bin`
- If the web UI assets were changed, also upload LittleFS via `pio run --target uploadfs`
- The dashboard firmware version is still taken from `FW_VERSION` in `src/main.cpp`

---

## Updating the Setup Portal AP Credentials

The WiFiManager setup-portal SSID and password can be changed from the dashboard without reflashing:

1. Open **Settings -> Gateway Network**
2. Edit the setup AP SSID and/or password
3. Click **Save AP Config**

These values are used the next time the gateway has to open the setup portal.

---

## Web Interface Access Control

The dashboard can be left open or protected with username/password credentials from the web interface.

### Stored in NVS

- web username
- password hash
- remember token

If credentials are forgotten, recover access by performing a **factory reset**.

---

## Factory Reset

Hold the **BOOT button (GPIO 0)** for **5 seconds** while the gateway is running, or trigger **Factory Reset** from the dashboard.

This clears:

- saved Wi-Fi / router credentials
- custom setup AP credentials
- web interface credentials
- paired node registry
- saved relay label assignments

The gateway then reboots and returns to setup mode.

---

## Key Configuration (`src/main.cpp`)

All main tuneable constants are near the top of `src/main.cpp`.

| Constant | Default | Description |
|----------|---------|-------------|
| `AP_SSID_DEFAULT` | `"ESP32-Mesh-Gateway"` | WiFiManager setup portal SSID |
| `AP_PASS_DEFAULT` | `"meshsetup"` | WiFiManager setup portal password |
| `WEB_PORT` | `80` | HTTP/WebSocket port |
| `WS_UPDATE_MS` | `2000` | Node/discovery push interval to browser |
| `WS_META_MS` | `10000` | Gateway meta push interval to browser |
| `RX_QUEUE_SIZE` | `30` | Incoming ESP-NOW packet queue depth |
| `GW_LED_PIN` | `38` | On-board WS2812B gateway status LED |

Timing constants shared with the nodes live in `include/mesh_protocol.h`.

---

## Serial Monitor

```bash
pio device monitor
```

Default baud rate: **115200**

Useful log prefixes:

| Prefix | Meaning |
|--------|---------|
| `[BOOT]` | Startup sequence |
| `[FS]` | LittleFS mount / file listing |
| `[WiFi]` | Wi-Fi connection events |
| `[ESP-NOW]` | ESP-NOW init and TX callbacks |
| `[DISC]` | Node discovery |
| `[PAIR]` | Pairing handshake |
| `[MESH]` | Node registration, relay/actuator state, reconnects |
| `[CFG]` | Gateway or node configuration activity |
| `[AUTH]` | Web interface authentication |
| `[WS]` | WebSocket client activity |
| `[OTA]` | Gateway OTA upload and reboot flow |

---

## WebSocket Message Reference

The dashboard communicates with the gateway over a single WebSocket at:

```text
ws://<gateway-ip>/ws
```

### Gateway -> Browser

| `type` | Description |
|--------|-------------|
| `update` | Full node list with online state, uptime, sensors, actuator state, and relay labels |
| `meta` | Gateway metadata such as firmware version, uptime, IP, AP SSID, OTA support |
| `discovered` | Nodes currently broadcasting in pairing mode |
| `pair_timeout` | Pairing window expired |
| `node_settings` | Settings schema + current values for one node |
| `node_sensor_schema` | Sensor schema for one node |
| `relay_labels_ack` | Relay label save/reset acknowledgement |
| `ap_config_ack` | Setup AP config save acknowledgement |
| `web_creds_ack` | Web credential save acknowledgement |
| `gw_portal_starting` | Gateway is restarting into WiFiManager portal mode |
| `gw_rebooting` | Gateway is about to reboot |
| `gw_factory_reset` | Factory reset acknowledged |
| `auth_required` / `auth_ok` / `auth_fail` / `session_expired` | Web interface auth flow |

### Browser -> Gateway

| `type` | Description |
|--------|-------------|
| `auth` | Authenticate the WebSocket connection |
| `actuator_cmd` | Toggle an actuator on a paired actuator node |
| `pair_cmd` | Pair a discovered node |
| `unpair_cmd` | Disconnect and forget a paired node |
| `rename_node` | Change a node display name |
| `reboot_node` | Reboot a selected node |
| `reboot_gw` | Reboot the gateway |
| `set_ap_config` | Save setup AP SSID/password |
| `set_web_credentials` | Save dashboard login credentials |
| `start_wifi_portal` | Re-open WiFiManager setup portal |
| `factory_reset` | Clear gateway data and reboot |
| `gw_led_toggle` | Enable/disable the gateway status LED |
| `node_settings_get` | Request node settings schema |
| `node_settings_set` | Update one node setting |
| `node_sensor_schema_get` | Request node sensor schema |
| `relay_labels_set` | Save per-relay labels on the gateway |
| `ping` | Keep-alive |

---

## File Layout

```text
gateway_v1/
|-- src/
|   `-- main.cpp
|-- include/
|   `-- mesh_protocol.h
|-- data/
|   |-- index.html
|   |-- js/
|   |   `-- app.js
|   `-- css/
|       `-- style.css
|-- partitions_8mb_noot.csv
|-- partitions_8mb_ota.csv
|-- platformio.ini
`-- README.md
```
