# ESP32 Relay Node v1

Firmware version: **1.1.0**
Target board: `esp32dev`

## Firmware Changelog
| Version | Notes |
|---------|-------|
| v1.0.0 | Initial relay actuator node release with 4-channel relay control over ESP-NOW mesh |
| v1.0.1 | Added per-node settings support for relay state persistence and status LED control, added actuator-state sync after reconnect/reboot, and fixed relay-state persistence so all 4 relays restore correctly after reboot |
| v1.0.2 | Added capacitive touch sensor support to the ESP32 Relay Node with relay-state sync for the web interface |
| v1.1.0 | Added gateway-managed Node OTA support, helper-AP download handling, OTA finalization/reboot flow, and validated relay-node OTA reconnect behavior |

## Hardware

| Item | Detail |
|------|--------|
| Board | ESP32-DevKitC (ESP-WROOM-32E) |
| Relay Module | 4-Way 5V 10A Active-Low Relay Module |
| Status LED | WS2812B on GPIO 5 |
| Pairing button | GPIO 16 (active-LOW, external pull-up) |
| Touch Sensor | TTP224 4-Way Capacitive Touch Sensor Module |

### Wiring

**Relay Module -> ESP32-DevKitC**

| Relay Module Pin | ESP32-DevKitC Pin |
|------------------|-------------------|
| JD-VCC | 5V external power source |
| VCC | 3.3V |
| GND | GND and shared GND with external 5V power source |
| IN1 | GPIO 26 |
| IN2 | GPIO 27 |
| IN3 | GPIO 32 |
| IN4 | GPIO 33 |

> **Note:** The relay module must be powered by an external 5V power source connected to the JD-VCC pin, and its GND must be shared with the ESP32 GND. The IN1-IN4 pins are active-LOW, meaning they activate the relay when driven LOW.

**WS2812B breakout -> ESP32-DevKitC**

| WS2812B Pin | ESP32-DevKitC Pin |
|-------------|-------------------|
| VCC | 5V |
| GND | GND |
| DATA-IN | GPIO 5 |
| DATA-OUT | Not connected |

**Capacitive Touch Sensor Module -> ESP32-DevKitC**

| TTP224 Pin | ESP32-DevKitC Pin |
|-----------|-------------------|
| VCC | 3.3V |
| GND | GND |
| OUT1 | GPIO 25 (Relay 1 control) |
| OUT2 | GPIO 4 (Relay 2 control) |
| OUT3 | GPIO 13 (Relay 3 control) |
| OUT4 | GPIO 14 (Relay 4 control) |

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
| `RELAY1_PIN` | `26` | Relay-1 control GPIO |
| `RELAY2_PIN` | `27` | Relay-2 control GPIO |
| `RELAY3_PIN` | `32` | Relay-3 control GPIO |
| `RELAY4_PIN` | `33` | Relay-4 control GPIO |
| `PAIR_BTN_PIN` | `16` | Pairing button GPIO (active-LOW) |
| `LED_PIN` | `5` | WS2812B data GPIO |
| `TOUCH1_PIN` | `25` | TTP224 capacitive touch sensor 1 |
| `TOUCH2_PIN` | `4` | TTP224 capacitive touch sensor 2 |
| `TOUCH3_PIN` | `13` | TTP224 capacitive touch sensor 3 |
| `TOUCH4_PIN` | `14` | TTP224 capacitive touch sensor 4 |
| `relay_active_high` | `false` | Active-HIGH relay support flag |

When deploying multiple nodes, give each a unique `NODE_NAME`.

---

## Build & Flash

From the node project directory:

```bash
# First time - erase flash for a clean start
pio run --target erase

# Compile and upload
pio run --target upload
```

No LittleFS upload is needed for actuator nodes.

After the initial USB flash, future compatible firmware builds can be delivered from the gateway web interface using **Node OTA**.

---

## First Boot & Pairing

1. Flash the firmware
2. On first boot the node prints:
   ```
   [PAIR]  No pairing data - hold button 3 s to pair.
   ```
3. Make sure the gateway is online
4. Hold **GPIO 16** (the pairing button) for **3 seconds** - the LED turns cyan and the node begins beaconing
5. The gateway detects the beacon and completes the handshake automatically
6. The LED turns solid green - the node is now paired and transmitting

At pair time the node sends its actuator state and settings data to the gateway so the dashboard can stay synchronized without extra manual configuration.

---

## Node OTA Update Support

When the gateway starts a Node OTA update for this device:

1. the node receives an OTA begin request over ESP-NOW
2. it connects to the temporary helper AP created by the gateway's ESP32-C3 coprocessor
3. it downloads the staged `firmware.bin`
4. it finalizes the new image, reboots, and re-registers automatically
5. relay control and dashboard synchronization resume after reconnect

Relay-node OTA has now been validated successfully with the current gateway-managed Node OTA workflow.

---

## Per-Node Settings

These settings are configurable from the dashboard (Gateway -> Connected Nodes -> Settings):

| Setting | Type | Default | Range / Options |
|---------|------|---------|-----------------|
| StatePersist | Bool | OFF | On / Off |
| Status LED | Bool | On | On / Off |

Settings are persisted in NVS under the namespace `"nodeconf"` - separate from pairing data, so a factory reset of the gateway does **not** wipe node settings.

- Enabling **StatePersist** enables relay-state save functionality, so all relay states are stored in flash and restored after reboot.
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
| `[ACTUATOR]` | Actuator state/schema transmit |
| `[PAIR]` | Pairing handshake steps |
| `[CFG]` | Settings load / save / apply |
| `[NVS]` | NVS read / write |
| `[MESH]` | Gateway-loss detection and re-registration |
| `[ESP-NOW]` | TX result callbacks |
| `[RELAY STATE]` | Relay state change function |
| `[OTA]` | Gateway-managed node OTA download, flash, and reboot flow |

---

## NVS Namespaces

| Namespace | Contents |
|-----------|----------|
| `"mesh"` | Pairing data (gateway MAC, channel, node ID) - cleared by factory reset |
| `"nodeconf"` | Node settings (relay persistence and LED enable) - **not** cleared by factory reset |
| `"relay"` | Relay state - **not** cleared by factory reset when persistence is enabled |
