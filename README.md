# ESPNow Mesh System

A lightweight, extensible smart-home mesh network built on the **ESP-NOW** protocol. An ESP32-S3 gateway bridges your Wi-Fi network to a fleet of ESP32 sensor and actuator nodes — all communicating over ESP-NOW with no router involvement for node-to-node traffic.

---

## ✨ Features

- **Zero-infrastructure node communication** — nodes talk directly to the gateway over ESP-NOW (IEEE 802.11 raw frames), no MQTT broker or Wi-Fi credentials required on the nodes
- **Button-press pairing** — hold the pairing button on any node for 3 seconds; the gateway discovers and registers it automatically
- **Live web dashboard** — served directly from the gateway over Wi-Fi; shows real-time sensor readings, node status, uptime, and controls
- **Per-node dynamic settings** — schema-on-wire design: each node self-describes its own configurable parameters; the gateway relays them to the browser without needing to know their meaning — fully extensible without gateway firmware changes
- **Remote management** — reboot any node, rename it, disconnect it, or reboot the gateway itself from the dashboard
- **Gateway network configuration** — change the Wi-Fi AP SSID/password from the dashboard; no reflashing needed
- **Offline detection** — nodes are marked offline after a configurable timeout; the gateway tracks last-seen timestamps
- **Gateway-loss recovery** — nodes detect when the gateway stops responding and automatically re-register when it comes back

---

## 📁 Repository Structure

```
espnow-mesh-system/
├── README.md                          ← you are here
├── esp32-gateway/
│   ├── README.md                      ← gateway overview & hardware setup
│   └── gateway_v1/
│       ├── src/main.cpp               ← gateway firmware (v1.6.0)
│       ├── include/mesh_protocol.h    ← Mesh Communication Protocol file (v2.0)
│       ├── data/                      ← LittleFS web assets
│       │   ├── index.html             ← Gateway Web Interface HTML file (v3.1)
│       │   ├── js/app.js              ← Gateway Web Interface JavaScript file (v3.1)
│       │   └── css/style.css          ← Gateway Web Interface CSS file (v3.0)
│       ├── platformio.ini
│       └── README.md                  ← build & flash instructions
└── esp32-nodes/
    ├── README.md                      ← node overview & pairing guide
    ├── sensor_nodes/
    │   ├── envo_mini_v1/
    │   │   ├── src/main.cpp           ← node firmware (v1.3.0)
    │   │   ├── include/mesh_protocol.h
    │   │   ├── platformio.ini
    │   │   └── README.md              ← wiring & configuration
    │   └── README.md
    └── actuator_nodes/
        └── README.md                  ← placeholder for future relay/actuator nodes
```

---

## 🏗️ Architecture Overview

```
  ┌─────────────────────────────────────────────────────┐
  │                 Your Wi-Fi Network                  │
  │                                                     │
  │   Browser ◄──── WebSocket (HTTP/WS) ────► Gateway  │
  └─────────────────────────────────────────────────────┘
                                  │
                            ESP-NOW (2.4 GHz)
                                  │
              ┌───────────────────┼───────────────────┐
              │                   │                   │
         Sensor Node 1      Sensor Node 2      Actuator Node
         (BMP280)            (SCD41 CO2)           (Relay)
```

- The **gateway** connects to your home Wi-Fi using WiFiManager (captive portal on first boot)
- **Nodes** never touch Wi-Fi — they only speak ESP-NOW to the gateway's MAC address on a fixed channel
- The **web dashboard** (AsyncWebServer + WebSocket) is served directly from the gateway's LittleFS flash partition

---

## 🔌 Hardware Used

| Role | Board | Flash |
|------|-------|-------|
| Gateway | ESP32-S3-DevKitC-1-N8R8 | 8 MB |
| Sensor Node | DFRobot Firebeetle 2 ESP32-E | 4 MB |

Any ESP32 or ESP32-S3 variant can be adapted with minor pin changes.

---

## 🚀 Quick Start

1. **Flash the gateway** — see [`esp32-gateway/gateway_v1/README.md`](esp32-gateway/gateway_v1/README.md)
2. **Flash a sensor node** — see [`esp32-nodes/sensor_nodes/bmp280_sensor/README.md`](esp32-nodes/sensor_nodes/bmp280_sensor/README.md)
3. Power on both devices
4. On first boot the gateway opens a Wi-Fi captive portal (`ESP32-Mesh-Setup` / `meshsetup`) — connect and enter your Wi-Fi credentials
5. Hold the pairing button on the sensor node for **3 seconds** — the gateway discovers and registers it
6. Open the gateway's IP address in a browser — the dashboard appears with live readings

---

## 🛠️ Built With

- [Arduino framework for ESP32](https://github.com/espressif/arduino-esp32) via PlatformIO
- [WiFiManager](https://github.com/tzapu/WiFiManager) — captive-portal Wi-Fi configuration
- [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) — non-blocking HTTP + WebSocket server
- [ArduinoJson](https://arduinojson.org/) — JSON serialisation for WebSocket messages
- [Adafruit BMP280 Library](https://github.com/adafruit/Adafruit_BMP280_Library) — temperature & pressure sensor driver
- [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel) — WS2812B RGB status LED

---

## 📜 License

MIT — see [LICENSE](LICENSE)