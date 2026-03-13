# Contributing to ESPNow Mesh System

Thank you for your interest in contributing. This document covers everything you need to build, modify, and extend the project — from setting up a development environment to opening a pull request.

---

## Table of Contents

- [Development Environment](#development-environment)
- [Repository Structure](#repository-structure)
- [Architecture Overview](#architecture-overview)
- [How to Contribute](#how-to-contribute)
- [Adding a New Sensor Node](#adding-a-new-sensor-node)
- [Extending the Protocol](#extending-the-protocol)
- [Working on the Gateway Firmware](#working-on-the-gateway-firmware)
- [Working on the Web Dashboard](#working-on-the-web-dashboard)
- [Pull Request Workflow](#pull-request-workflow)
- [Ground Rules](#ground-rules)
- [Built With](#built-with)

---

## Development Environment

**Requirements:**
- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) or the PlatformIO IDE extension for VS Code
- A USB cable per device for flashing and serial monitoring

No additional toolchain installation is needed — PlatformIO manages the ESP32 toolchain, framework, and all library dependencies automatically.

---

## Repository Structure

```
espnow-mesh-system/
├── README.md
├── CONTRIBUTING.md                    ← you are here
├── esp32-gateway/
│   ├── README.md                      ← gateway overview & hardware setup
│   └── gateway_v1/
│       ├── src/main.cpp               ← gateway firmware (v1.8.0)
│       ├── include/mesh_protocol.h    ← shared protocol definitions (v3.0)
│       ├── data/                      ← LittleFS web assets
│       │   ├── index.html             ← web interface HTML (v3.4)
│       │   ├── js/app.js              ← web interface JavaScript (v3.4)
│       │   └── css/style.css          ← web interface CSS (v3.3)
│       ├── platformio.ini
│       └── README.md                  ← build & flash instructions
└── esp32-nodes/
    ├── README.md                      ← node overview & pairing guide
    ├── sensor_nodes/
    │   ├── envo_mini_v1/
    │   │   ├── src/main.cpp           ← node firmware (v2.0.0)
    │   │   ├── include/mesh_protocol.h
    │   │   ├── platformio.ini
    │   │   └── README.md
    │   └── README.md
    └── actuator_nodes/
        └── README.md                  ← placeholder for future relay/actuator nodes
```

---

## Architecture Overview

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
       (Envo Mini V1)        (SCD41 CO2)           (Relay)
     Temp · Pressure       CO2 · Temp · RH        (planned)
     Humidity · Light
```

**Key design principles:**

- **Schema-driven sensors** — nodes self-describe their sensors to the gateway via `MSG_SENSOR_SCHEMA` at pair time (labels, units, precision). The gateway and dashboard have zero hardcoded sensor knowledge. Adding sensors to a node requires only node firmware changes.
- **Schema-driven settings** — nodes self-describe their configurable parameters via `MSG_SETTINGS_SCHEMA`. The gateway relays the schema to the browser, which renders the settings panel dynamically. Adding a setting to a node requires only node firmware changes.
- **Binary ESP-NOW frames** — all node↔gateway communication uses packed C structs defined in `mesh_protocol.h`. Keep this file identical across gateway and node projects.
- **AsyncWebServer + WebSocket** — the dashboard is served from LittleFS and communicates with the gateway over a single persistent WebSocket connection.

---

## How to Contribute

Good starting points for contributions:

| Area | Ideas |
|------|-------|
| **New sensor node** | CO₂ (SCD41), soil moisture, PIR motion, door/window contact |
| **Actuator node** | 4-channel relay board — gateway protocol stubs already exist |
| **Gateway firmware** | OTA update support, multi-channel mesh, improved NVS error handling |
| **Web dashboard** | Sensor history charts, mobile layout improvements, dark/light theme toggle |
| **Tooling** | Web-based flasher using ESP Web Tools, GitHub Actions CI for PlatformIO builds |
| **Hardware** | Custom gateway PCB, custom sensor node PCB |

---

## Adding a New Sensor Node

The schema-driven protocol means new sensor nodes require **no changes to the gateway or dashboard**. The steps are:

1. Copy an existing node directory (e.g. `envo_mini_v1/`) as a starting point
2. Add your sensor driver and initialisation in `setup()`
3. Add a `SensorDef` entry for each measurement in `getSensorDefs()`:
   ```cpp
   out[i].id        = 4;           // unique sensor ID for this node
   out[i].precision = 1;           // decimal places shown in dashboard
   strncpy(out[i].label, "CO2",  SENSOR_LABEL_LEN - 1);
   strncpy(out[i].unit,  "ppm",  SENSOR_UNIT_LEN  - 1);
   i++;
   ```
4. Add the corresponding reading in `sendSensorData()` using the same `id`:
   ```cpp
   sd.readings[sd.count++] = { .id = 4, .value = co2ppm };
   ```
5. Add per-node settings in `loadSettings()` / `saveSettings()` / `getSettingsDefs()` if needed
6. Update `NODE_NAME` and the `README.md` for the new node directory
7. Add the node to the table in `esp32-nodes/sensor_nodes/README.md`

See [`esp32-nodes/sensor_nodes/README.md`](esp32-nodes/sensor_nodes/README.md) for a more detailed guide.

---

## Extending the Protocol

`mesh_protocol.h` is the contract between the gateway and all nodes. Before modifying it:

- **Any struct size change is a breaking change** — bump the major version (`v3.0` → `v4.0`) and document the change
- **The file must be kept identical** across `esp32-gateway/gateway_v1/include/` and every node's `include/` directory — a mismatch causes silent communication failures
- **Adding a new message type** is non-breaking as long as existing struct sizes are unchanged — bump the minor version only
- Document every new message type and struct field with a comment in the header

Current version: **v3.0**

---

## Working on the Gateway Firmware

Build and flash from `esp32-gateway/gateway_v1/`:

```bash
# First time — erase flash for a clean start
pio run --target erase

# Upload web assets to LittleFS (only needed when data/ files change)
pio run --target uploadfs

# Compile and upload firmware
pio run --target upload

# Serial monitor
pio device monitor
```

Key files:
- `src/main.cpp` — all gateway logic; tuneable constants at the top
- `include/mesh_protocol.h` — shared binary frame definitions
- `data/` — web dashboard assets served from LittleFS

Log prefixes for serial debugging:

| Prefix | Meaning |
|--------|---------|
| `[BOOT]` | Startup sequence |
| `[FS]` | LittleFS mount / file listing |
| `[WiFi]` | Wi-Fi connection events |
| `[ESP-NOW]` | ESP-NOW init and TX callbacks |
| `[DISC]` | Node discovery (beacon detected) |
| `[PAIR]` | Pairing handshake steps |
| `[MESH]` | Node registration |
| `[SENS]` | Sensor schema registration and incoming readings |
| `[CFG]` | Node settings get / set |
| `[WS]` | WebSocket client connect / disconnect |

---

## Working on the Web Dashboard

The dashboard is a single-page vanilla JS + CSS application served from LittleFS. There is no build step — edit the files in `data/` and upload them with:

```bash
pio run --target uploadfs
```

Key files:
- `data/index.html` — page structure and all HTML
- `data/js/app.js` — WebSocket handling, DOM rendering, all UI logic
- `data/css/style.css` — all styling

The dashboard communicates with the gateway over a WebSocket at `ws://<ip>/ws`. See the WebSocket message reference in [`esp32-gateway/gateway_v1/README.md`](esp32-gateway/gateway_v1/README.md) for the full list of message types in both directions.

The sensor and settings panels are fully schema-driven — `app.js` has no hardcoded sensor or setting names. When working on the dashboard, keep this property intact.

---

## Pull Request Workflow

1. Fork the repository
2. Create a branch: `git checkout -b feat/your-feature-name`
3. Make your changes — keep gateway, node, and web interface changes in separate commits where possible
4. **Test on real hardware** before opening a PR — serial monitor logs from both gateway and node are helpful to include in the PR description
5. Update the relevant `README.md` files if you add a new node type, new settings, or new WebSocket messages
6. Open a pull request with a clear description of what changed and why

---

## Ground Rules

- **Keep `mesh_protocol.h` in sync** — the copy in `esp32-gateway/` and the copy in each node directory must always be identical.
- **New sensor nodes should not require gateway changes** — the schema handshake exists precisely to avoid this. If your sensor contribution requires gateway firmware changes, reconsider the design.
- **Prefer small, focused PRs** — a new sensor node in one PR, a dashboard fix in another. Mixed-scope PRs are harder to review and harder to revert if something breaks.
- **No hardcoded sensor names in the gateway or dashboard** — all sensor knowledge belongs in the node firmware via the schema.

---

## Built With

- [Arduino framework for ESP32](https://github.com/espressif/arduino-esp32) via PlatformIO
- [WiFiManager](https://github.com/tzapu/WiFiManager) — captive-portal Wi-Fi configuration
- [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) — non-blocking HTTP + WebSocket server
- [ArduinoJson](https://arduinojson.org/) — JSON serialisation for WebSocket messages
- [Adafruit BMP280 Library](https://github.com/adafruit/Adafruit_BMP280_Library) — temperature & pressure sensor driver
- [DHT sensor library](https://github.com/adafruit/DHT-sensor-library) — DHT22 humidity sensor driver
- [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel) — WS2812B RGB status LED