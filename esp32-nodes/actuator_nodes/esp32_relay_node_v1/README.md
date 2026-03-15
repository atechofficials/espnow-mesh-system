# ESP32 Relay Node v1

Firmware version: **1.0.1**
Target board: `esp32dev`

## Firmware Changelog
| Version | Notes |
|---------|-------|
| v1.0.0 | Initial relay actuator node release with 4-channel relay control over ESP-NOW mesh |
| v1.0.1 | Added per-node settings support for Relay State Persistence and Status LED control, added actuator-state sync after reconnect/reboot, and fixed relay-state persistence so all 4 relays restore correctly after reboot |

## Hardware

| Item | Detail |
|------|--------|
| Board | ESP32-DevKitC (ESP-WROOM-32E) |
| Actuator 1 | 5V Active-Low Relay-1 |
| Actuator 2 | 5V Active-Low Relay-2 |
| Actuator 3 | 5V Active-Low Relay-3 |
| Actuator 4 | 5V Active-Low Relay-4 |
| Status LED | WS2812B on GPIO 5 |
| Pairing button | GPIO 16 (active-LOW, external pull-up) |

### Wiring

**Relay Module → ESP32-DevKitC**

| Relay Module Pin | ESP32-DevKitC Pin |
|------------|-----------------|
| JD-VCC | 5V Ext Power Source |
| VCC | 3.3V |
| GND | GND also share GND with Ext 5V Power Source |
| IN1 | GPIO 26 |
| IN2 | GPIO 27 |
| IN3 | GPIO 32 |
| IN4 | GPIO 33 |

> **Note:** The relay module must be powered by an external 5V power source connected to the JD-VCC pin, and its GND must be shared with the ESP32's GND. The IN1-IN4 pins are active-LOW, meaning they will activate the relay when driven LOW.

**WS2812B breakout → ESP32-DevKitC**

| DHT22 Pin | ESP32-DevKitC Pin |
|-----------|-----------------|
| VCC | 5V |
| GND | GND |
| DATA-IN | GPIO 5 |
| DATA-OUT | Not Connected |

---

## Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

| Library | Version |
|---------|---------|
| Adafruit NeoPixel | 1.15.4 |

Framework libraries used directly: `Preferences`, `WiFi`, `Wire`.

## Configuration (`src/main.cpp`)

Change these defines before flashing:

| Constant | Default | Description |
|----------|---------|-------------|
| `NODE_NAME` | `"Relay-Node-1"` | Display name shown in the dashboard (max 15 chars) |
| `RELAY1_PIN` | `26` | Relay-1 Control GPIO |
| `RELAY2_PIN` | `27` | Relay-2 Control GPIO |
| `RELAY3_PIN` | `32` | Relay-3 Control GPIO |
| `RELAY4_PIN` | `33` | Relay-4 Control GPIO |
| `PAIR_BTN_PIN` | `16` | Pairing button GPIO (active-LOW) |
| `LED_PIN` | `5` | WS2812B data GPIO |

When deploying multiple nodes, give each a unique `NODE_NAME`.

---

## Build & Flash

From the node project directory:

```bash
# First time — erase flash for a clean start
pio run --target erase

# Compile and upload
pio run --target upload
```

No LittleFS upload is needed for sensor nodes (no web assets).

---

## First Boot & Pairing

1. Flash the firmware
2. On first boot the node prints:
   ```
   [PAIR]  No pairing data — hold button 3 s to pair.
   ```
3. Make sure the gateway is online
4. Hold **GPIO 16** (the pairing button) for **3 seconds** — the LED turns cyan and the node begins beaconing
5. The gateway detects the beacon and completes the handshake automatically
6. The LED turns solid green — the node is now paired and transmitting

At pair time the node sends its sensor schema to the gateway. The gateway and dashboard adapt automatically — no configuration required on the gateway side.

---

## Actuator Schema

---

## Per-Node Settings

These settings are configurable from the dashboard (Gateway → Connected Nodes → ⚙ Settings):

| Setting | Type | Default | Range / Options |
|---------|------|---------|-----------------|
| StatePersist | Bool | OFF | On / Off |
| Status LED | Bool | On | On / Off |

Settings are persisted in NVS under the namespace `"nodeconf"` — separate from pairing data, so a factory reset of the gateway does **not** wipe node settings.

- Enabling **StatePersist** enables the Relay State Save Functionality, which means all the relay states gets saved into the device flash and survives reboot and restored.
- Disabling **Status LED** turns off the WS2812B except during pairing and boot sequences.

---

## Serial Monitor

```bash
pio device monitor
```

Default baud rate: **115200**. Log prefixes:

| Prefix | Meaning |
|--------|---------|
| `[BOOT]` | Startup sequence |
| `[ACTUATOR]` | Actuator schema transmit |
| `[PAIR]` | Pairing handshake steps |
| `[CFG]` | Settings load / save / apply |
| `[NVS]` | NVS read / write |
| `[MESH]` | Gateway-loss detection and re-registration |
| `[ESP-NOW]` | TX result callbacks |
| `[RELAY STATE]` | Relay State Change Function |
---

## NVS Namespaces

| Namespace | Contents |
|-----------|----------|
| `"mesh"` | Pairing data (gateway MAC, channel, node ID) — cleared by factory reset |
| `"nodeconf"` | Node settings (temp unit, send interval, LED enable, light sensitivity) — **not** cleared by factory reset |
| `"relay"` | Relay State — **not** cleared by factory reset |
