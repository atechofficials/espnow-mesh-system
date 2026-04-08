# ESP32 Gateway

The gateway is the heart of the ESPNow Mesh System. It runs on an **ESP32-S3** and bridges your Wi-Fi network with the ESP-NOW mesh while serving a live web dashboard, managing paired nodes, optionally exposing a built-in **BMP280/BME280 room sensor**, supporting a local **MQTT bridge** with **Home Assistant MQTT auto-discovery**, supporting **web-based gateway OTA firmware updates**, coordinating **gateway-managed Node OTA updates** for sensor, actuator, and hybrid nodes with a companion **ESP32-C3 coprocessor**, and providing an **Offline Mode AP** so the dashboard stays reachable even when your home router is unavailable.

---

## Hardware

| Item | Detail |
|------|--------|
| Main Board | ESP32-S3-DevKitC-1-N8R8 |
| Companion Helper | ESP32-C3 coprocessor for Node OTA and future helper tasks |
| Flash | 8 MB (Quad) |
| PSRAM | 8 MB (Octal) |
| Status LED | WS2812B on GPIO 38 (on-board) |
| Factory reset button | Board-dependent and configured by `RESET_BTN_PIN` in `gateway_v1/include/user_config.h` |

The current gateway hardware release line now tracks four single-layer, thick-trace, THT-friendly PCB variants under `gateway_v1/hardware/`:

| Variant | Main MCU board | Helper MCU board |
|---------|----------------|------------------|
| `ESP32_Mesh_Gateway_v1A` | Seeed Studio XIAO ESP32-S3 | ESP32-C3 Super Mini |
| `ESP32_Mesh_Gateway_v1B` | Seeed Studio XIAO ESP32-S3 | Seeed Studio XIAO ESP32-C3 |
| `ESP32_Mesh_Gateway_v1C` | Seeed Studio XIAO ESP32-S3 | DFRobot Beetle ESP32-C3 |
| `ESP32_Mesh_Gateway_v1D` | Waveshare ESP32-S3-DevKit-C-N8R8 | ESP32-C3 Super Mini |

All four variants include solder pads for an optional built-in **BMP280** or **BME280** gateway-side room sensor. The older ESP32-S3 Super Mini based carrier should now be treated as deprecated because the current gateway firmware line expects an 8 MB class ESP32-S3 target.

---

## Responsibilities

- Connects to your home Wi-Fi using a captive portal (WiFiManager) on first boot or when reconfigured
- Can also run in an **ESP32-S3-hosted Offline Mode AP** with separate offline AP credentials
- Automatically falls back to the Offline Mode AP when the router Wi-Fi link is lost and returns to router mode when the saved router SSID is visible again
- Accepts ESP-NOW registrations from nodes and assigns them IDs
- Receives sensor data, heartbeats, and actuator states from paired nodes
- Optionally reads a gateway-side BMP280/BME280 room sensor and exposes it as a dedicated dashboard card instead of a paired node
- Persists built-in sensor display settings such as temperature unit and altitude reference for sea-level pressure calculation
- Publishes optional MQTT discovery/state topics for the gateway, built-in sensor, and supported paired nodes
- Routes supported Home Assistant / MQTT commands back into the same gateway and node control flow used by the dashboard
- Publishes MQTT temperature data in degrees Celsius for Home Assistant and leaves unit conversion to Home Assistant itself
- Forwards dashboard commands to the correct node (pair, unpair, reboot, actuator toggle, settings change)
- Serves the web dashboard from LittleFS over HTTP/WebSocket
- Stores gateway configuration, web credentials, paired node records, node hardware-config IDs, and relay label assignments in NVS
- Supports **gateway self-OTA** from the web interface with validation, hardware-config ID checking, progress reporting, and automatic reboot
- Coordinates **Gateway OTA**, **coprocessor OTA**, and **Node OTA** so the ESP32-C3 helper cannot be reserved by conflicting update flows at the same time
- Keeps **gateway OTA**, **coprocessor OTA**, and **Node OTA** available while the gateway is serving the dashboard through Offline Mode
- Supports both dashboard-triggered and physical-button-triggered factory reset flows, including graceful node unpairing before reset, and LED feedback during the hold sequence
- Supports **Hybrid nodes** with capability-aware state handling, actuator-schema sync, RFID config sync, and RFID scan-event forwarding to the web UI

---

## LED Status Codes

| Colour / Pattern | Meaning |
|------------------|---------|
| Solid white | Boot sequence in progress |
| Dim blue | Gateway operational |
| Green flash | Activity / success pulse |
| Orange flash | Node disconnect feedback |
| Dark-orange blink | Physical factory reset button is being held |
| Solid red | Factory reset hold threshold reached; reset is being confirmed |
| Off | Gateway LED disabled from the web UI |

The factory-reset LED sequence temporarily overrides the normal gateway LED owner and still appears even if the user has disabled the status LED from the web dashboard.

---

## Firmware Changelog

| Version | Notes |
|---------|-------|
| v1.6.0 | Added per-node dynamic settings, remote reboot support, gateway network config from dashboard, and factory reset support |
| v1.7.1 | Maintenance release with minor bug fixes |
| v1.8.0 | Added schema-driven sensor support, generic sensor reading protocol, and multi-sensor node compatibility |
| v1.8.1 | Added gateway status LED toggle support from the web UI with persistent state |
| v1.8.2 | Added ESP32 actuator-node support and improved actuator pairing/state/settings sync |
| v1.8.3 | Added per-relay label assignment from the web interface for relay nodes |
| v1.9.0 | Added **web-based gateway OTA firmware update**, OTA partition layout, upload validation, progress/error feedback, and automatic reboot after successful flash |
| v2.0.0 | Added **gateway-managed Node OTA** for supported sensor and actuator nodes, introduced the ESP32-C3 gateway coprocessor workflow, added helper-assisted node firmware staging/delivery, and extended OTA progress/reconnect tracking in the dashboard |
| v2.0.1 | Added OTA **hardware configuration ID** checks for both gateway OTA and node OTA, persisted node hardware IDs across gateway reboot, and improved OTA mismatch/error reporting in the serial logs and web UI |
| v2.1.0 | Added first-class **Hybrid node** support, introduced the RFID-enabled Hybrid Relay Node flow, added capability-aware actuator/RFID handling, and improved relay-state restore plus gateway OTA UI feedback after reboot |
| v2.1.1 | Improved the **web dashboard UX** with RFID scan toasts, mobile responsive layout fixes, custom reboot confirmation popup, better popup styling on small screens, overlay scroll-lock fixes, and improved UI asset refresh behavior |
| v2.1.2 | Added documentation/support for the **Gateway v1.0A** PCB release, aligned the gateway-helper UART mapping with the new PCB layout, and kept the gateway firmware line in sync with the latest hardware release |
| v2.1.3 | Added gateway-side max-node pairing-capacity enforcement, added dismissible dashboard feedback when pairing is attempted after the gateway is already full, and hardened node-registry restore so reduced `MESH_MAX_NODES` test builds fail safely instead of corrupting memory |
| v2.1.4 | Increased the shared node-name limit from 15 to 24 visible characters, updated gateway discovery/registry/rename handling for longer node names, added backward-compatible NVS restore support while saving new node records in the expanded-name format, and allowed fresh nodes to appear with MAC-suffixed default names for easier identification before manual rename |
| v2.2.0 | Polished gateway-side discovery timing so available nodes appear and expire more predictably, aligned the release docs with the new Gateway v1A-v1D PCB family, and documents the move toward `user_config.h` for user-facing firmware configuration |
| v2.3.1 | Hardened OTA coordination so Node OTA, Gateway OTA, and coprocessor OTA cannot fight over the ESP32-C3 helper, surfaced the shared **Coprocessor Busy** feedback across both gateway MCU targets, fixed a Node OTA helper-target regression, and auto-clears stale OTA panel state after backend cleanup |
| v2.4.0 | Added **Gateway Offline Mode** on the ESP32-S3 with manual setup-portal selection, automatic router-loss fallback, automatic router-return recovery, reconnect-safe dashboard notifications, runtime physical factory reset handling via the configurable `RESET_BTN_PIN`, and a dedicated factory-reset RGB LED sequence |
| v2.4.1 | Updates gateway factory reset so all currently paired nodes are sent a graceful unpair/disconnect command before the gateway wipes its own state, preventing nodes from treating factory reset like a temporary gateway reboot |
| v2.5.0 | Added optional gateway built-in BMP280/BME280 sensor support with dedicated dashboard/settings handling, aligned sensor-type selection with build-time hardware configuration, and hardened ESP32-C3 Super Mini helper Wi-Fi behavior for more stable Node OTA AP sessions |
| v2.6.4 | Added MQTT bridging and Home Assistant MQTT auto-discovery, exposed gateway reboot/factory-reset controls in Home Assistant, kept supported sensor/actuator/hybrid node controls synchronized through MQTT, and clears retained MQTT discovery/state during gateway factory reset so the gateway and child devices disappear cleanly from Home Assistant |

---

## Contents

```text
gateway_v1/
|-- src/
|   `-- main.cpp              <- gateway firmware
|-- include/
|   |-- mesh_protocol.h       <- shared protocol definitions (keep in sync with nodes)
|   `-- coproc_ota_protocol.h <- shared S3 <-> C3 OTA transport definitions
|-- coprocessor_esp32c3/      <- helper firmware used for Node OTA delivery
|-- data/                     <- LittleFS web assets (upload with --target uploadfs)
|   |-- index.html
|   |-- js/
|   |   `-- app.js
|   `-- css/
|       `-- style.css
|-- partitions_8mb_noot.csv   <- legacy no-OTA layout kept for reference
|-- partitions_8mb_ota.csv    <- active OTA-capable partition layout
|-- platformio.ini
`-- README.md
```

---

## Current Release Notes

- Gateway firmware version: **v2.6.4**
- Gateway coprocessor firmware version: **v0.3.1**
- Shared helper transport: `coproc_ota_protocol.h` **v1.1.0**
- Shared mesh protocol: `mesh_protocol.h` **v3.3.2**
- User configuration header: `user_config.h` **v1.1.2**
- Web UI assets:
  - `app.js` **v4.8.0**
  - `index.html` **v4.2.0**
  - `style.css` **v3.7.0**
- Active partition layout: **`partitions_8mb_ota.csv`**

### MQTT and Home Assistant notes

- MQTT is optional and is only active while the gateway is connected to a router
- Home Assistant MQTT discovery is published automatically once the gateway connects to the configured broker
- Gateway reboot and factory reset are exposed as Home Assistant MQTT button entities
- Relay label settings are intentionally not exposed through Home Assistant, because Home Assistant already supports local renaming of switches and buttons
- Gateway built-in sensor and paired sensor-node temperatures are published in Celsius only; Home Assistant handles Fahrenheit conversion if the user prefers that display unit

---

### Gateway build environments

The current `gateway_v1/platformio.ini` release line exposes explicit build environments for the documented hardware combinations:

- `gateway_v1a` - Seeed Studio XIAO ESP32-S3 + ESP32-C3 Super Mini
- `gateway_v1b` - Seeed Studio XIAO ESP32-S3 + Seeed Studio XIAO ESP32-C3
- `gateway_v1c` - Seeed Studio XIAO ESP32-S3 + DFRobot Beetle ESP32-C3
- `gateway_v1d` - Waveshare ESP32-S3-DevKit-C-N8R8 + ESP32-C3 Super Mini
- `development` - custom ESP32-S3-DevKitC-1-N8R8 + DFRobot Beetle ESP32-C3 validation combo

See [gateway_v1/README.md](gateway_v1/README.md) for build, flashing, gateway OTA, Node OTA, coprocessor setup, and configuration details.
