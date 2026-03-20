# ESP32 Gateway

The gateway is the heart of the ESPNow Mesh System. It runs on an **ESP32-S3** and bridges your Wi-Fi network with the ESP-NOW mesh while serving a live web dashboard, managing paired nodes, supporting **web-based gateway OTA firmware updates**, and coordinating **gateway-managed Node OTA updates** with a companion **ESP32-C3 coprocessor**.

---

## Hardware

| Item | Detail |
|------|--------|
| Main Board | ESP32-S3-DevKitC-1-N8R8 |
| Companion Helper | ESP32-C3 coprocessor for Node OTA and future helper tasks |
| Flash | 8 MB (Quad) |
| PSRAM | 8 MB (Octal) |
| Status LED | WS2812B on GPIO 38 (on-board) |
| Boot / Reset button | GPIO 0 (on-board BOOT button, used for factory reset) |

---

## Responsibilities

- Connects to your home Wi-Fi using a captive portal (WiFiManager) on first boot or when reconfigured
- Accepts ESP-NOW registrations from nodes and assigns them IDs
- Receives sensor data, heartbeats, and actuator states from paired nodes
- Forwards dashboard commands to the correct node (pair, unpair, reboot, actuator toggle, settings change)
- Serves the web dashboard from LittleFS over HTTP/WebSocket
- Stores gateway configuration, web credentials, paired node records, and relay label assignments in NVS
- Stores gateway configuration, web credentials, paired node records, node hardware-config IDs, and relay label assignments in NVS
- Supports **gateway self-OTA** from the web interface with validation, hardware-config ID checking, progress reporting, and automatic reboot
- Supports **Node OTA** by validating node role and hardware-config markers, staging node firmware, handing delivery to the ESP32-C3 helper, tracking node reconnects, and reporting OTA progress back to the dashboard

---

## LED Status Codes

| Colour / Pattern | Meaning |
|------------------|---------|
| Solid white | Boot sequence in progress |
| Dim blue | Gateway operational |
| Green flash | Activity / success pulse |
| Orange flash | Node disconnect feedback |
| Off | Gateway LED disabled from the web UI |

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

- Gateway firmware version: **v2.0.1**
- Gateway coprocessor firmware version: **v0.1.0**
- Shared helper transport: `coproc_ota_protocol.h` **v1.0.0**
- Shared mesh protocol: `mesh_protocol.h` **v3.2.1**
- Web UI assets:
  - `app.js` v4.0
  - `index.html` v3.7
  - `style.css` v3.5
- Active partition layout: **`partitions_8mb_ota.csv`**

---

See [gateway_v1/README.md](gateway_v1/README.md) for build, flashing, gateway OTA, Node OTA, coprocessor setup, and configuration details.
