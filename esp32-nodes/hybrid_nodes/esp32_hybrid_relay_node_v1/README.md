# ESP32 Hybrid Relay Node v1

Firmware version: **0.3.2**
Target board: `esp32dev`

This is the first **Hybrid node** in the ESPNow Mesh System. It starts from the existing 4-relay actuator architecture, keeps the same touch-input and relay-control behavior, and adds an **RC522 RFID reader** so saved RFID cards can apply predefined relay scenes directly on the node.

The firmware registers itself as `NODE_HYBRID`, reports Hybrid capabilities to the gateway, exposes actuator schema/state like a normal relay node, and also exchanges RFID-card configuration with the dashboard. Supported builds can be updated through the same **gateway-managed Node OTA** flow used by the other node families. The current release line uses the working **Arduino_MFRC522v2** library, keeps the MQTT/Home Assistant bridge compatible through the gateway, and tightens heartbeat/liveness behavior for a more stable and responsive control flow.

## Firmware Changelog
| Version | Notes |
|---------|-------|
| v0.1.0 | Initial Hybrid Relay Node release with 4 relays, 4 TTP224 touch inputs, RC522 RFID card actions, dashboard RFID management, and gateway-managed Hybrid Node OTA support |
| v0.1.1 | Added periodic RC522 health checks with automatic reader recovery after long-uptime stalls, and updated the recommended RC522 reset wiring to a safer GPIO for reliable USB flashing |
| v0.1.2 | Restores the node status RGB LED to enabled when the node is unpaired from the gateway and saves that LED state back to NVS so pairing/status indication is visible again when the node is later re-paired |
| v0.1.3 | Updated to the `mesh_protocol.h v3.3.1` line with support for 24-character node names and automatic first-boot default naming that appends the last 4 MAC characters for easier node identification without aggressive truncation |
| v0.2.0 | Introduced `user_config.h` for Hybrid-node board and feature configuration, and documents the current release-line guidance for any future ESP32-C3 Super Mini based Hybrid variants |
| v0.3.2 | Switched to the working `Arduino_MFRC522v2` RFID library, fixed heartbeat/liveness timing for more stable gateway connectivity and a snappier UI feel, and keeps MQTT/Home Assistant control flows synchronized through the gateway |

---

## Hardware

| Item | Detail |
|------|--------|
| Board | ESP32-DevKitC (ESP-WROOM-32E) |
| Relay Module | 4-Way 5V 10A active-LOW relay module |
| Touch Sensor | TTP224 4-Way Capacitive Touch Sensor Module |
| RFID Reader | RC522 / MFRC522 SPI RFID module |
| Status LED | WS2812B on GPIO 22 |
| Pairing button | GPIO 16 (active-LOW, internal pull-up) |

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

> **Note:** The relay module is active-LOW. A relay turns ON when its control pin is driven LOW and turns OFF when driven HIGH.

**TTP224 -> ESP32-DevKitC**

| TTP224 Pin | ESP32-DevKitC Pin |
|------------|-------------------|
| VCC | 3.3V |
| GND | GND |
| OUT1 | GPIO 25 |
| OUT2 | GPIO 4 |
| OUT3 | GPIO 13 |
| OUT4 | GPIO 14 |

**WS2812B -> ESP32-DevKitC**

| WS2812B Pin | ESP32-DevKitC Pin |
|-------------|-------------------|
| VCC | 5V |
| GND | GND |
| DATA-IN | GPIO 22 |

**RC522 / MFRC522 -> ESP32-DevKitC**

| RC522 Pin | ESP32-DevKitC Pin |
|-----------|-------------------|
| SDA / SS | GPIO 17 |
| SCK | GPIO 18 |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| RST | GPIO 21 |
| 3.3V | 3.3V |
| GND | GND |

> **Note:** The current recommended RC522 `RST` wiring uses **GPIO21**. Earlier experiments with `GPIO2` could interfere with reliable USB flashing on some hardware setups while the RFID module remained connected.

---

## Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

| Library | Version |
|---------|---------|
| Adafruit NeoPixel | ^1.12.3 |
| ArduinoJson | ^7.4.2 |
| Arduino_MFRC522v2 | 2.0.6 |

Framework libraries used directly: `Preferences`, `WiFi`, `SPI`.

---

## Configuration (`include/user_config.h`)

The current release line moves user-facing pin maps, board choices, and naming defaults into `user_config.h`. If your checkout predates that refactor, the same symbols may still live near the top of `src/main.cpp`.

Key values to review before flashing:

| Constant | Default | Description |
|----------|---------|-------------|
| `NODE_NAME` | `"RFID-Hybrid-Node"` | Base display name shown in the dashboard (up to 24 visible chars; fresh unrenamed nodes append the last 4 MAC characters automatically) |
| `RELAY1_PIN` | `26` | Relay-1 control GPIO |
| `RELAY2_PIN` | `27` | Relay-2 control GPIO |
| `RELAY3_PIN` | `32` | Relay-3 control GPIO |
| `RELAY4_PIN` | `33` | Relay-4 control GPIO |
| `TOUCH1_PIN` | `25` | TTP224 touch output 1 |
| `TOUCH2_PIN` | `4` | TTP224 touch output 2 |
| `TOUCH3_PIN` | `13` | TTP224 touch output 3 |
| `TOUCH4_PIN` | `14` | TTP224 touch output 4 |
| `PAIR_BTN_PIN` | `16` | Pairing button GPIO (active-LOW) |
| `LED_PIN` | `5` | WS2812B data GPIO |
| `RFID_CS_PIN` | `17` | RC522 chip-select pin |
| `RFID_RST_PIN` | `21` | RC522 reset pin |
| `HW_CONFIG_ID` | `"0x0D"` | Hardware configuration ID embedded in firmware and reported to the gateway for OTA compatibility checks |

When deploying multiple nodes, give each node a unique `NODE_NAME` when practical. Fresh nodes already append the last 4 MAC characters automatically, and you can still rename them later from the gateway dashboard.

---

## Build & Flash

From the node project directory:

```bash
# First time - erase flash for a clean start
pio run --target erase

# Compile and upload
pio run --target upload
```

No LittleFS upload is needed for Hybrid nodes.

After the initial USB flash, future compatible firmware builds can be delivered from the gateway web interface using **Node OTA**.

If USB flashing fails while the RC522 reader is connected, double-check that the reader `RST` line is wired to the recommended non-strapping GPIO and not to an ESP32 boot-related pin on your hardware variant.

---

## First Boot & Pairing

1. Flash the firmware
2. On first boot the node prints:
   ```
   [PAIR]  No pairing data - hold button 3 s to pair.
   ```
3. Make sure the gateway is online
4. Hold **GPIO 16** for **3 seconds** - the LED turns cyan and the node begins beaconing
5. The gateway detects the beacon and completes the handshake automatically
6. The LED turns solid green - the node is now paired and transmitting

At pair time the node sends its actuator state, settings data, firmware version, hardware configuration ID, and Hybrid capability information to the gateway so the dashboard can render the correct Hybrid controls without extra manual setup.

---

## RFID Card Actions

The RC522 reader allows the gateway/dashboard to store up to **8 saved RFID card slots** on the node.

Current workflow:

1. Swipe an unknown card on the RC522 reader
2. The node sends an RFID scan event to the gateway
3. In the dashboard, choose the scanned card from the RFID Card Actions section
4. Set the desired ON/OFF state for each relay
5. Save the card action back to the node

When that saved card is presented again, the node:

- matches the RFID UID to a saved slot
- applies the saved relay mask as an absolute relay-state overwrite
- reports the new actuator state back to the gateway
- emits a scan event so the dashboard stays synchronized

Unknown cards do not change relay state until a card action is saved for them.

The current firmware also performs periodic RC522 health checks at runtime and will reinitialize the reader automatically if the RFID interface stalls after long uptime, reducing the need to manually reboot the node just to restore card detection.

---

## Node OTA Update Support

When the gateway starts a Node OTA update for this device:

1. the node receives an OTA begin request over ESP-NOW
2. it connects to the temporary helper AP created by the gateway's ESP32-C3 coprocessor
3. it downloads the staged `firmware.bin`
4. it finalizes the new image, reboots, and re-registers automatically
5. actuator control, RFID features, and dashboard synchronization resume after reconnect

The gateway validates both the `HYBRID` role marker and the matching `HW_CONFIG_ID` before the helper delivery flow is started.

Validated behaviors include:

- version upgrades
- version downgrades
- rejection of sensor firmware uploaded to a Hybrid target
- rejection of actuator firmware uploaded to a Hybrid target
- successful reconnect after Hybrid-node OTA

When MQTT is enabled on the gateway, this node is also exposed through Home Assistant MQTT discovery. Supported relay controls, supported settings, reboot, and unpair stay synchronized with the gateway dashboard.

Relay label settings are intentionally not exposed to Home Assistant, because Home Assistant already lets users rename switches and buttons locally.

---

## Per-Node Settings

These settings are configurable from the dashboard (Gateway -> Connected Nodes -> Settings):

| Setting | Type | Default | Range / Options |
|---------|------|---------|-----------------|
| StatePersist | Bool | OFF | On / Off |
| Status LED | Bool | On | On / Off |

Settings are persisted in NVS under the node configuration namespace.

- Enabling **StatePersist** stores relay states in flash and restores them after reboot.
- Disabling **Status LED** turns off the WS2812B except during pairing and boot sequences.

---

## Serial Monitor

```bash
pio device monitor
```

Default baud rate: **115200**. Useful log prefixes:

| Prefix | Meaning |
|--------|---------|
| `[BOOT]` | Startup sequence |
| `[ACTUATOR]` | Actuator state/schema transmit |
| `[PAIR]` | Pairing handshake steps |
| `[CFG]` | Settings load / save / apply |
| `[NVS]` | NVS read / write |
| `[ESP-NOW]` | TX result callbacks |
| `[RELAY STATE]` | Relay state change function |
| `[RFID]` | RC522 reader status, scan events, and saved card actions |
| `[OTA]` | Gateway-managed Hybrid node OTA download, flash, and reboot flow |
