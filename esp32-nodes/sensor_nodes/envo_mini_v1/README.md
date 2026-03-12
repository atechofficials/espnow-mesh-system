# Envo Mini V1 Node

Firmware version: **1.3.0**
Target board: `dfrobot_firebeetle2_esp32e`

Reads temperature and barometric pressure from a **Bosch BMP280** sensor over I²C and transmits the readings to the gateway every configurable interval over ESP-NOW.

---

## Hardware

| Item | Detail |
|------|--------|
| Board | DFRobot Firebeetle 2 ESP32-E |
| Sensor | Bosch BMP280 (temperature + pressure) |
| Sensor interface | I²C |
| Status LED | WS2812B on GPIO 5 |
| Pairing button | GPIO 27 (active-LOW, internal pull-up) |

### Wiring (BMP280 breakout → Firebeetle 2)

| BMP280 Pin | Firebeetle 2 Pin |
|------------|-----------------|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

The BMP280 I²C address is auto-detected (tries `0x76` then `0x77`).

---

## Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

| Library | Version |
|---------|---------|
| Adafruit BMP280 Library | 2.6.8 |
| Adafruit Unified Sensor | 1.1.15 |
| Adafruit NeoPixel | 1.15.4 |

Framework libraries used directly: `Preferences`, `WiFi`, `Wire`.

---

## Configuration (`src/main.cpp`)

Change these defines before flashing:

| Constant | Default | Description |
|----------|---------|-------------|
| `NODE_NAME` | `"BMP280-Node-1"` | Display name shown in the dashboard (max 15 chars) |
| `BMP_I2C_SDA` | `21` | I²C SDA GPIO |
| `BMP_I2C_SCL` | `22` | I²C SCL GPIO |
| `PAIR_BTN_PIN` | `27` | Pairing button GPIO (active-LOW) |
| `LED_PIN` | `5` | WS2812B data GPIO |

When deploying multiple nodes, give each a unique `NODE_NAME`.

---

## Build & Flash

From the `bmp280_sensor/` directory:

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
4. Hold **GPIO 27** (the pairing button) for **3 seconds** — the LED turns cyan and the node begins beaconing
5. The gateway detects the beacon and completes the handshake automatically
6. The LED turns solid green — the node is now paired and transmitting

---

## Per-Node Settings

These settings are configurable from the dashboard (Gateway → Connected Nodes → ⚙ Settings):

| Setting | Type | Default | Range / Options |
|---------|------|---------|-----------------|
| Temp Unit | Enum | °C | °C / °F |
| Send Intvl | Integer | 10 s | 5–60 s (step 5) |
| Status LED | Bool | On | On / Off |

Settings are persisted in NVS under the namespace `"nodeconf"` — separate from pairing data, so a factory reset of the gateway does **not** wipe node settings.

Changing **Temp Unit** affects the value transmitted to the gateway; the conversion happens on the node before transmission.
Changing **Send Intvl** takes effect immediately — no reboot required.
Disabling **Status LED** turns off the WS2812B except during pairing and boot sequences.

---

## Serial Monitor

```bash
pio device monitor
```

Default baud rate: **115200**. Log prefixes:

| Prefix | Meaning |
|--------|---------|
| `[BOOT]` | Startup sequence |
| `[BMP]` | Sensor read + transmit result |
| `[PAIR]` | Pairing handshake steps |
| `[CFG]` | Settings load/save/apply |
| `[NVS]` | NVS read/write |
| `[MESH]` | Gateway-loss detection and re-registration |
| `[ESP-NOW]` | TX result callbacks |

---

## NVS Namespaces

| Namespace | Contents |
|-----------|----------|
| `"mesh"` | Pairing data (gateway MAC, channel, node ID) — cleared by factory reset |
| `"nodeconf"` | Node settings (temp unit, send interval, LED enable) — **not** cleared by factory reset |