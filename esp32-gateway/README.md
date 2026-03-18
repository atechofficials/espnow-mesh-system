# ESP32 Gateway

The gateway is the heart of the ESPNow Mesh System. It runs on an **ESP32-S3** and bridges your Wi-Fi network with the ESP-NOW mesh while serving a live web dashboard, managing paired nodes, and now supporting **web-based gateway OTA firmware updates**.

---

## Hardware

| Item | Detail |
|------|--------|
| Board | ESP32-S3-DevKitC-1-N8R8 |
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
- Supports **gateway self-OTA** from the web interface with validation, progress reporting, and automatic reboot

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

---

## Contents

```text
gateway_v1/
|-- src/
|   `-- main.cpp              <- gateway firmware
|-- include/
|   `-- mesh_protocol.h       <- shared protocol definitions (keep in sync with nodes)
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

- Gateway firmware version: **v1.9.0**
- Web UI assets:
  - `app.js` v3.8
  - `index.html` v3.6
  - `style.css` v3.5
- Active partition layout: **`partitions_8mb_ota.csv`**

---

See [gateway_v1/README.md](gateway_v1/README.md) for build, flashing, OTA usage, and configuration details.
