# Gateway v1 - Build, Flash, and OTA Guide

Firmware version: **2.1.4**  
Target board: `esp32-s3-devkitc1-n8r8`

Gateway helper coprocessor: **ESP32-C3 firmware v0.1.1**

---

## Highlights in v2.1.4
- Increased the shared node-name field from 16 bytes to 25 bytes (24 visible characters + NUL) for beacon and registration messages, enabling clearer default node names, MAC-suffixed first-boot node names, and longer user-defined names while remaining well within ESP-NOW payload limits

## Highlights in v2.1.3

- Adds a gateway-side **max node capacity** check before a new pairing flow is started
- Shows a dismissible dashboard popup when the user tries to connect another node after the gateway is already full
- Re-shows the capacity warning on repeated over-limit pairing attempts instead of silently failing
- Hardens gateway node-registry restore so temporary low-`MESH_MAX_NODES` test builds do not corrupt runtime memory on boot
- Continues support for the current **ESP32-C3 gateway coprocessor**, Hybrid-node handling, RFID dashboard flow, gateway OTA, and gateway-managed Node OTA

## Highlights in v2.1.2

- Adds the first **ESP32 Mesh System Gateway v1.0A** hardware release to the repository under `hardware/`
- Documents a home-fabrication-friendly gateway PCB built around off-the-shelf **ESP32-S3 Super Mini** and **ESP32-C3 Super Mini** development boards
- Aligns the documented ESP32-S3 <-> ESP32-C3 UART wiring with the new PCB layout:
  - **ESP32-S3 TX GPIO4 -> ESP32-C3 RX GPIO0**
  - **ESP32-S3 RX GPIO5 -> ESP32-C3 TX GPIO1**
- Continues the current gateway OTA, Node OTA, Hybrid-node, and mobile-web-UI feature set from the v2.1.1 release line

## Highlights in v2.1.1

- Continues the **gateway-managed Node OTA** workflow for supported sensor, actuator, and hybrid nodes
- Continues first-class **Hybrid node** handling with capability-aware actuator/RFID synchronization
- Continues support for the **ESP32 Hybrid Relay Node v1** and its RFID card-action workflow in the dashboard
- Adds RFID scan toasts in the dashboard for **new card detection** and **known card action execution**
- Improves the **mobile web UI** with better top-bar responsiveness, mobile sidebar gateway actions, and improved side-nav behavior in portrait and landscape orientations
- Replaces the browser reboot confirmation with a custom **Reboot Gateway** popup consistent with the Factory Reset flow
- Improves popup styling, warning-icon visibility, small-screen landscape fit, and background scroll locking for side menus and dialogs
- Preserves actuator state across gateway reboot so relay cards recover correctly after restart
- Improves gateway OTA success/reboot feedback in the web interface
- Continues support for the **ESP32-C3 gateway coprocessor**, `coproc_ota_protocol.h`, gateway web credentials, relay label assignment, gateway LED control, and schema-driven node settings

---

## Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) or the PlatformIO IDE extension for VS Code
- USB cable connected to the ESP32-S3-DevKitC-1-N8R8
- USB cable connected to the ESP32-C3 coprocessor when flashing or debugging the Node OTA helper firmware

The new **Gateway v1.0A** PCB release in `hardware/` instead uses off-the-shelf **ESP32-S3 Super Mini** and **ESP32-C3 Super Mini** boards soldered onto a single-layer carrier PCB.

---

## Gateway v1.0A Hardware Release

The repository now includes the first gateway PCB release under `hardware/ESP32_Mesh_Gateway_v1A/`.

Current board-level highlights:

- uses off-the-shelf **ESP32-S3 Super Mini** and **ESP32-C3 Super Mini** development boards
- single-layer, thick-trace layout intended to be easy to fabricate and hand-assemble
- relies on the ESP32-S3 Super Mini's built-in **ARGB LED**, so no separate WS2812B is placed on the PCB
- includes a place to connect a **BME280** module so the gateway can later report its own basic room temperature and humidity (**firmware support still in development**)
- includes an external **5V JST-style power connector**

The same `hardware/` tree also includes schematic PDFs and a `Development_Resources/` folder with dev-board pinouts and MCU reference documents.

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

This layout is required for the web-based Gateway OTA update feature and leaves the gateway firmware on an OTA-capable base while the ESP32-C3 helper handles node-firmware delivery.

---

## First-Time Flash / Migration to the OTA-Capable Gateway Line

If you are coming from an older gateway build that used the non-OTA partition table, the **first upgrade to the current OTA-capable gateway line must be done over USB**.

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

To use **Node OTA**, also flash the ESP32-C3 helper once from the `coprocessor_esp32c3/` directory:

```bash
cd coprocessor_esp32c3
pio run --target erase
pio run --target upload
pio device monitor
```

See [coprocessor_esp32c3/README.md](coprocessor_esp32c3/README.md) for the helper-specific hardware, transport, and runtime details.

If you change only the dashboard assets later, run:

```bash
pio run --target uploadfs
```

If you change only the firmware later and want to use USB instead of OTA, run:

```bash
pio run --target upload
```

---

## ESP32-C3 Gateway Coprocessor

The Node OTA workflow uses a dedicated **ESP32-C3 coprocessor** connected to the ESP32-S3 gateway.

During a node update, the gateway:

1. validates and stages the uploaded node `firmware.bin`
2. transfers that firmware to the ESP32-C3 helper over UART
3. tells the helper to start a temporary OTA access point
4. instructs the selected node to join that helper AP
5. waits for the node to download, flash, reboot, and re-register

The coprocessor also reports OTA helper status back to the gateway so the web interface can show live progress.

For the current **Gateway v1.0A** PCB release, the UART link is routed as:

- **ESP32-S3 TX GPIO4 -> ESP32-C3 RX GPIO0**
- **ESP32-S3 RX GPIO5 -> ESP32-C3 TX GPIO1**

The `hardware/` directory also includes schematic PDFs, KiCad source files, and development-reference material for that board revision.

Shared transport definitions between the gateway and helper live in:

- `include/coproc_ota_protocol.h`
- `coprocessor_esp32c3/include/coproc_ota_protocol.h`

Keep those copies synchronized.

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

Once an OTA-capable gateway build is already installed, the gateway can update its **own firmware** directly from the dashboard.

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
- validates the uploaded gateway hardware configuration marker (`GWHWCFG`)
- checks OTA slot size and image type
- flashes the inactive OTA slot
- reports progress/errors in the web UI
- reboots into the new firmware automatically on success

### Notes

- Upload **`firmware.bin`**, not `bootloader.bin`, not `partitions.bin`
- If the web UI assets were changed, also upload LittleFS via `pio run --target uploadfs`
- The dashboard firmware version is still taken from `FW_VERSION` in `src/main.cpp`

---

## Node OTA Update from the Web Interface

The gateway can also update a paired **sensor node**, **actuator node**, or **hybrid node** from the dashboard.

### Supported flow

1. Build the target node firmware and locate its `firmware.bin`
2. Open the dashboard in a browser
3. Go to the **Node OTA Update** section
4. Select the target node and upload the matching node `firmware.bin`
5. Wait for the gateway to stage the file, arm the ESP32-C3 helper, and start the temporary helper AP
6. The target node downloads the staged image, flashes it, reboots, and re-registers automatically

### What the gateway checks

- uploaded image role matches the selected node type (`SENSOR`, `ACTUATOR`, or `HYBRID`)
- uploaded node hardware configuration marker matches the selected node's reported `HW_CONFIG_ID`
- firmware metadata is readable before starting delivery
- helper staging finishes successfully on the ESP32-C3
- the node accepts OTA mode and reconnects after reboot

### Notes

- Upload **node `firmware.bin`** from the node project, not bootloader or partition files
- The node does **not** need your home Wi-Fi credentials; it only joins the temporary helper AP for the OTA window
- Wrong-role uploads, wrong-hardware uploads, and gateway firmware uploaded to the node OTA route are rejected before helper staging starts
- Supported and verified flows now include same-version reflashing, upgrades, downgrades, sensor-node OTA, relay-node OTA, hybrid-node OTA, and node OTA after gateway reboot

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

Node OTA transport and helper constants are shared with the ESP32-C3 helper through `include/coproc_ota_protocol.h`.

If you are working against the Gateway v1.0A PCB instead of the original dev-kit wiring, keep the UART pin assignments in `src/main.cpp` aligned with the PCB routing described above.

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
| `[C3]` | ESP32-C3 helper handshake, staging, and helper status |
| `[NODE OTA]` | Node OTA job lifecycle, node reconnect detection, and completion |

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
| `pair_capacity_full` | The gateway is already at max node capacity and rejected the requested pair action |
| `node_settings` | Settings schema + current values for one node |
| `node_sensor_schema` | Sensor schema for one node |
| `relay_labels_ack` | Relay label save/reset acknowledgement |
| `ap_config_ack` | Setup AP config save acknowledgement |
| `web_creds_ack` | Web credential save acknowledgement |
| `gw_portal_starting` | Gateway is restarting into WiFiManager portal mode |
| `gw_rebooting` | Gateway is about to reboot |
| `gw_factory_reset` | Factory reset acknowledged |
| Node OTA progress/status messages | Helper staging, helper AP state, target-node reconnect, and completion/error feedback during Node OTA |
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
| node OTA upload actions | Upload a compatible node `firmware.bin` and start gateway-managed Node OTA for the selected node |
| `relay_labels_set` | Save per-relay labels on the gateway |
| `ping` | Keep-alive |

---

## File Layout

```text
gateway_v1/
|-- src/
|   `-- main.cpp
|-- include/
|   |-- mesh_protocol.h
|   `-- coproc_ota_protocol.h
|-- coprocessor_esp32c3/
|   |-- src/
|   |   `-- main.cpp
|   |-- include/
|   |   `-- coproc_ota_protocol.h
|   `-- platformio.ini
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
