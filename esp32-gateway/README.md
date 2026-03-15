# ESP32 Gateway

The gateway is the heart of the ESPNow Mesh System. It runs on an **ESP32-S3** and acts as the bridge between your Wi-Fi network and the ESP-NOW mesh — serving a live web dashboard while simultaneously listening for sensor readings, heartbeats, and pairing requests from nodes.

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

- Connects to your home Wi-Fi using a captive portal (WiFiManager) on first boot
- Accepts ESP-NOW registrations from nodes and assigns them IDs
- Receives sensor data, heartbeats, and relay states from paired nodes
- Forwards remote commands from the dashboard to the correct node (reboot, relay toggle, settings change)
- Serves the web dashboard from LittleFS over HTTP/WebSocket
- Stores gateway configuration (AP SSID, AP password) in NVS so it survives reboots
- Stores paired node records in NVS so the mesh reassembles after a power cycle without re-pairing

---

## LED Status Codes

| Colour / Pattern | Meaning |
|------------------|---------|
| Solid white (dim) | Booting |
| Slow blue pulse | Connecting to Wi-Fi / captive portal open |
| Solid green | Online, at least one node connected |
| Slow green pulse | Online, no nodes connected |
| Solid red | Fatal error |

---

## Firmware Changelog

| Version | Notes |
|---------|-------|
| v1.6.0 | Added per-node dynamic settings, remote reboot support, gateway network config from dashboard, and factory reset support |
| v1.7.1 | Maintenance release. Minor bug fixes. |
| v1.8.0 | Added schema-driven sensor support, generic sensor reading protocol, and multi-sensor node compatibility |
| v1.8.1 | added gateway status LED toggle support from the Web UI (with persistent state) |
| v1.8.2 | Added ESP32 actuator-node support, updated gateway logic for actuator pairing/state/settings flows, improved actuator state sync after reconnect/reboot |
---

## Contents

```
gateway_v1/
├── src/
│   └── main.cpp          ← gateway firmware
├── include/
│   └── mesh_protocol.h   ← shared protocol definitions (keep in sync with nodes)
├── data/                 ← LittleFS web assets (upload with --target uploadfs)
│   ├── index.html
│   ├── js/
│   │   └── app.js
│   └── css/
│       └── style.css
└── platformio.ini
```

---

See [`gateway_v1/README.md`](gateway_v1/README.md) for full build, flash, and configuration instructions.