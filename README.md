# ESPNow Mesh System

A wireless smart-home sensor network that runs entirely on your local network — no cloud, no subscription, no internet connection required. An ESP32-S3 gateway connects to your Wi-Fi and talks to a fleet of sensor nodes around your home, then serves a live dashboard you can open in any browser.

---

## ✨ What It Does

- **Live sensor dashboard** — open the gateway's IP address in any browser to see real-time readings from all your nodes: temperature, humidity, pressure, light level, and more
- **Wireless sensor nodes** — place nodes anywhere in range; they communicate directly with the gateway over ESP-NOW (no Wi-Fi credentials needed on the nodes)
- **Button-press pairing** — hold the pairing button on a node for 3 seconds; the gateway discovers and registers it automatically
- **Remote management** — reboot nodes, rename them, adjust their settings, and control the gateway — all from the dashboard
- **No cloud, no account** — everything runs on your local network; your sensor data never leaves your home
- **Self-healing mesh** — if the gateway reboots, nodes detect this and reconnect automatically; no manual intervention needed

---

## 🔌 What You Need

| Item | Details |
|------|---------|
| **Gateway** | ESP32-S3-DevKitC-1-N8R8 (1×) |
| **Sensor node** | DFRobot Firebeetle 2 ESP32-E (1× per node) |
| **Sensors (per node)** | Bosch BMP280 · DHT22 · TEMT6000 |
| **USB cables** | One per device for flashing |
| **Power** | USB 5V for gateway; USB or battery for nodes |

> **Coming soon:** pre-assembled PCBs and a one-click web flasher so no tools or technical knowledge are required.

---

## 🚀 Getting Started

### Step 1 — Flash the gateway

Follow the instructions in [`esp32-gateway/gateway_v1/README.md`](esp32-gateway/gateway_v1/README.md).

### Step 2 — Flash a sensor node

Follow the instructions in [`esp32-nodes/sensor_nodes/envo_mini_v1/README.md`](esp32-nodes/sensor_nodes/envo_mini_v1/README.md).

### Step 3 — First boot

1. Power on the gateway
2. On first boot it creates a Wi-Fi access point called **`ESP32-Mesh-Setup`** (password: `meshsetup`)
3. Connect to it from your phone or laptop — a setup page opens automatically
4. Select your home Wi-Fi and enter the password
5. The gateway connects and its IP address appears in the serial monitor (e.g. `http://192.168.1.141/`)

### Step 4 — Pair a sensor node

1. Power on the sensor node
2. Hold the **pairing button** for **3 seconds** — the LED turns cyan
3. The gateway discovers the node and completes the handshake automatically
4. The LED turns solid green — the node is paired and transmitting

### Step 5 — Open the dashboard

Open the gateway's IP address in any browser on your local network. You'll see live readings from all paired nodes updating every 10 seconds.

---

## 📡 Available Sensor Nodes

| Node | Measures | Firmware |
|------|---------|---------|
| Envo Mini v1 | Temperature · Atmospheric Pressure · Humidity · Ambient Light | v2.0.1 |

## Available Actuator Nodes
| Node | Actuators | Firmware |
| ESP32 Relay Node v1 | 4-Relays | v1.0.1 |

---

## 💡 LED Status Guide

### Gateway

| LED | Meaning |
|-----|---------|
| Solid white (dim) | Booting |
| Slow blue pulse | Connecting to Wi-Fi |
| Solid green | Online, nodes connected |
| Slow green pulse | Online, no nodes connected |

### Sensor Nodes

| LED | Meaning |
|-----|---------|
| Fast white flash | Booting |
| Slow cyan pulse | Pairing mode — waiting for gateway |
| Solid green | Paired and sending data |
| Slow amber pulse | Gateway lost — trying to reconnect |

---

## ❓ Troubleshooting

**Node won't pair**
- Make sure the gateway is fully booted and online (solid or pulsing green LED) before starting the pairing process
- Hold the pairing button for a full 3 seconds until the LED turns cyan
- Keep the node within a few metres of the gateway during pairing

**Dashboard shows a node as offline**
- Check the node is powered on and its LED is solid green
- If the gateway was recently rebooted, the node reconnects automatically within about 30 seconds

**Gateway won't connect to Wi-Fi**
- The captive portal reopens automatically if the saved credentials fail
- To reset Wi-Fi credentials completely, hold the **BOOT button** on the gateway for 5 seconds — this performs a factory reset

**Sensor readings look wrong**
- Temperature and pressure come from the BMP280 — make sure it is wired correctly (SDA → GPIO 21, SCL → GPIO 22)
- Humidity comes from the DHT22 (DATA → GPIO 16) — allow 2–3 minutes after power-on for the sensor to stabilise
- Light level comes from the TEMT6000 (SIG → GPIO 36) — adjust the Light Sensitivity setting in the node settings panel if readings seem too low or too high

---

## 🤝 Contributing & Development

Interested in adding a new sensor node, improving the dashboard, or building an actuator node? See [CONTRIBUTING.md](CONTRIBUTING.md) for the full development guide.

---

## 📜 License

MIT — see [LICENSE](LICENSE)