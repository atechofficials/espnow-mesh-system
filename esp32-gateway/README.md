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

## Versions

| Version | Notes |
|---------|-------|
| v1.6.0 | Per-node dynamic settings (schema-on-wire); remote reboot; gateway network config from dashboard; factory reset |
| v1.7.1 | N/A |
| v1.8.0 | Schema-driven sensor system (MSG_SENSOR_SCHEMA); generic sensor readings protocol; multi-sensor node support (DHT22, TEMT6000) |

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