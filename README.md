# ESPNow Mesh System

A local-first ESP32 smart-home system for **wireless sensing and control**.

This project lets you build a small home mesh using **ESP32-based nodes** and a central **ESP32-S3 gateway**.  
The gateway connects to your Wi-Fi, hosts a browser-based dashboard, communicates with nearby nodes over **ESP-NOW**, and now uses a companion **ESP32-C3 coprocessor** to handle **gateway-managed Node OTA updates** and future helper tasks.

That means you can:

- monitor room conditions like temperature, humidity, pressure, and light
- control actuator devices like relays from a web dashboard
- pair nodes without entering Wi-Fi credentials on each node
- keep everything running on your own local network

No cloud account. No subscription. No internet dependency for normal use.

---

## What This Project Can Do

### Live monitoring
Open the gateway's IP address in your browser to view live sensor readings from all paired nodes.

### Local control
Turn relays on and off directly from the dashboard.

### Easy pairing
Put a node into pairing mode and the gateway can discover and register it automatically.

### Node settings from the browser
Change node settings such as:
- Status LED enable/disable
- Relay state persistence
- Sensor-specific options

### Gateway-managed Node OTA
Upload compatible sensor-node or actuator-node firmware from the gateway web interface and let the gateway update the selected node without connecting that node to your home Wi-Fi.

### Automatic recovery
If the gateway or node reboots, the system reconnects automatically and restores state where supported.

---

## Current Project Status

The project already supports both **sensor nodes** and **actuator nodes**.

### Available right now

| Type | Device | Status |
|------|--------|--------|
| Gateway | ESP32-S3 Gateway | Working |
| Gateway Helper | ESP32-C3 Coprocessor | Working |
| Sensor Node | Envo Mini v1 | Working |
| Actuator Node | ESP32 Relay Node v1 | Working |

### Current node capabilities

**Envo Mini v1**
- Temperature
- Atmospheric pressure
- Humidity
- Ambient light

**ESP32 Relay Node v1**
- 4 independent relay outputs
- Relay control from dashboard
- Relay state persistence across reboot
- Status LED setting from dashboard

### Current OTA capabilities

- Gateway self-OTA from the browser
- Gateway-managed OTA for supported sensor nodes
- Gateway-managed OTA for supported actuator nodes

---

## Why ESP-NOW?

Most DIY smart-home systems put every device directly on Wi-Fi.

This project takes a different approach:

- the **gateway** joins your home Wi-Fi
- the **nodes** talk to the gateway using **ESP-NOW**
- nodes do not need your Wi-Fi password
- pairing is simpler for small embedded devices
- the system stays lightweight and local

This makes the setup especially useful for small ESP32 devices that only need to talk to one central controller.

---

## How It Works

```text
Browser
   │
   │  Dashboard
   ▼
ESP32-S3 Gateway
   │
   │  ESP-NOW
   ├── Sensor Nodes
   └── Actuator Nodes
```

The gateway acts as the bridge between:
- your browser over Wi-Fi
- your ESP32 nodes over ESP-NOW

For **Node OTA**, the gateway temporarily hands firmware delivery to the on-board **ESP32-C3 coprocessor**, which stages the selected node firmware, starts a temporary helper access point, and serves the firmware image to the target node while the main gateway continues managing the mesh and dashboard.

---

## Hardware Used

### Gateway
- ESP32-S3-DevKitC-1-N8R8
- ESP32-C3 helper coprocessor for Node OTA delivery and future gateway-side expansion

### Sensor node reference board
- DFRobot FireBeetle 2 ESP32-E

### Sensor node reference sensors
- BMP280
- DHT22
- TEMT6000

### Actuator node reference board
- ESP32 Dev Module

### Actuator node reference output
- 4-channel active-LOW relay module

---

## Quick Start

### 1. Flash the gateway
Start with the gateway firmware:

[`esp32-gateway/gateway_v1/README.md`](esp32-gateway/gateway_v1/README.md)

For **Node OTA** support, also flash the ESP32-C3 gateway coprocessor once from:

[`esp32-gateway/gateway_v1/coprocessor_esp32c3/`](esp32-gateway/gateway_v1/coprocessor_esp32c3/)

### 2. Flash a node
Choose one:

Sensor node:
[`esp32-nodes/sensor_nodes/envo_mini_v1/README.md`](esp32-nodes/sensor_nodes/envo_mini_v1/README.md)

Actuator node:
[`esp32-nodes/actuator_nodes/esp32_relay_node_v1/README.md`](esp32-nodes/actuator_nodes/esp32_relay_node_v1/README.md)

### 3. Power on the gateway
On first boot, the gateway helps you connect it to your home Wi-Fi.

### 4. Pair a node
Put a node into pairing mode and let the gateway detect it.

### 5. Open the dashboard
Visit the gateway IP in your browser and start using the system.

---

## Web Dashboard Features

The current dashboard supports:

- viewing all paired nodes
- seeing live sensor readings
- controlling relay outputs
- opening node settings
- renaming nodes
- rebooting nodes remotely
- updating supported nodes with Node OTA
- disconnecting nodes
- changing gateway settings
- launching Wi-Fi setup mode
- factory reset options

The dashboard is served directly by the gateway itself.

---

## Who This Project Is For

This project is useful if you are:

### A maker or hobbyist
You want a local smart-home system without cloud dependency.

### A student or learner
You want to explore ESP32 networking, embedded firmware, and browser-based control panels.

### A developer
You want a modular ESP32 project with:
- gateway firmware
- sensor firmware
- actuator firmware
- a shared protocol
- a live dashboard
- room for future expansion

---

## For Developers

The project is already structured for further development.

If you want to:
- add new sensor nodes
- add new actuator nodes
- improve the gateway
- improve the dashboard
- extend the protocol
- contribute fixes or new features

start here:

[`CONTRIBUTING.md`](CONTRIBUTING.md)

More detailed documentation is available in the subproject README files.

---

## Repository Layout

```text
espnow-mesh-system/
├── README.md
├── CONTRIBUTING.md
├── esp32-gateway/
└── esp32-nodes/
```

### Main sections

- [`esp32-gateway/`](esp32-gateway/)  
  Gateway documentation and firmware

- [`esp32-nodes/`](esp32-nodes/)  
  Sensor and actuator node firmware

---

## Documentation Map

If you're just exploring:

- Gateway overview: [`esp32-gateway/README.md`](esp32-gateway/README.md)
- Nodes overview: [`esp32-nodes/README.md`](esp32-nodes/README.md)

If you're building hardware:

- Gateway firmware guide: [`esp32-gateway/gateway_v1/README.md`](esp32-gateway/gateway_v1/README.md)
- Sensor node guide: [`esp32-nodes/sensor_nodes/envo_mini_v1/README.md`](esp32-nodes/sensor_nodes/envo_mini_v1/README.md)
- Relay node guide: [`esp32-nodes/actuator_nodes/esp32_relay_node_v1/README.md`](esp32-nodes/actuator_nodes/esp32_relay_node_v1/README.md)

If you're contributing code:

- Contribution guide: [`CONTRIBUTING.md`](CONTRIBUTING.md)

---

## Project Direction

The project currently supports:
- local sensor monitoring
- local relay control
- browser-based node management
- gateway-managed Node OTA for supported sensor and relay nodes

Planned future growth may include:
- more sensor node types
- more actuator node types
- hybrid sensor + actuator nodes
- improved dashboard views and charts
- cleaner onboarding and flashing experience

---

## License

MIT License

See [`LICENSE`](LICENSE)
```
