# Envo Mini V1 Node

Firmware version: **2.1.2**
Target board: `dfrobot_firebeetle2_esp32e`

Reads temperature and barometric pressure from a **Bosch BMP280**, humidity from a **DHT22**, and ambient light level from a **TEMT6000** phototransistor. Transmits all readings to the gateway over ESP-NOW using the schema-driven sensor protocol, so the node self-describes its sensors to the gateway at pair time and no gateway changes are needed when sensors are added or removed.

This node also supports the **gateway-managed Node OTA** workflow introduced with the ESP32-S3 gateway `v2.0.0` and ESP32-C3 helper firmware `v0.1.0`. Current releases also report this node's hardware configuration ID so the gateway can reject incompatible sensor firmware before the OTA session starts, and remain fully compatible with the Hybrid-capable gateway/dashboard line built on `mesh_protocol.h v3.3.0`.

## Firmware Changelog
| Version | Notes |
|---------|-------|
| v2.0.0 | Added two more sensors: DHT22 for relative humidity and TEMT6000 for ambient light |
| v2.0.1 | Added more serial-monitor debugging messages |
| v2.1.0 | Added gateway-managed Node OTA support, helper-AP download handling, OTA finalization/reboot flow, and improved OTA status reporting |
| v2.1.1 | Added `HW_CONFIG_ID` reporting/firmware markers for hardware-safe OTA validation and compatibility checks from the gateway |
| v2.1.2 | Updated to the `mesh_protocol.h v3.3.0` line with capability-aware registration for compatibility with the Hybrid-node-capable gateway release while preserving existing sensor behavior |

---

## Hardware

| Item | Detail |
|------|--------|
| Board | DFRobot Firebeetle 2 ESP32-E |
| Sensor 1 | Bosch BMP280 (temperature + pressure), I2C |
| Sensor 2 | DHT22 (humidity), single-wire |
| Sensor 3 | TEMT6000 phototransistor module (ambient light), ADC |
| Status LED | WS2812B on GPIO 5 |
| Pairing button | GPIO 27 (active-LOW, internal pull-up) |

### Wiring

**BMP280 breakout -> Firebeetle 2**

| BMP280 Pin | Firebeetle 2 Pin |
|------------|------------------|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

The BMP280 I2C address is auto-detected (tries `0x76` then `0x77`).

**DHT22 -> Firebeetle 2**

| DHT22 Pin | Firebeetle 2 Pin |
|-----------|------------------|
| VCC | 3V3 |
| GND | GND |
| DATA | GPIO 16 |

**TEMT6000 module -> Firebeetle 2**

| TEMT6000 Pin | Firebeetle 2 Pin |
|--------------|------------------|
| VCC | 3V3 |
| GND | GND |
| SIG | GPIO 36 (ADC1_CH0) |

The TEMT6000 module has an on-board 10K emitter resistor, so the signal pin is always actively driven and does not require an external pull-down.

> **Note:** GPIO 36 on the ESP32 is an input-only pin. The firmware works around a known ESP32 hardware issue where ADC readings are corrupted during ESP-NOW transmissions by collecting 64 samples per reading cycle and discarding zero-valued samples before averaging.

---

## Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

| Library | Version |
|---------|---------|
| Adafruit BMP280 Library | 2.6.8 |
| Adafruit Unified Sensor | 1.1.15 |
| Adafruit NeoPixel | 1.15.4 |
| DHT sensor library | 1.4.6 |

Framework libraries used directly: `Preferences`, `WiFi`, `Wire`.

---

## Configuration (`src/main.cpp`)

Change these defines before flashing:

| Constant | Default | Description |
|----------|---------|-------------|
| `NODE_NAME` | `"BMP280-Node-1"` | Display name shown in the dashboard (max 15 chars) |
| `BMP_I2C_SDA` | `21` | I2C SDA GPIO |
| `BMP_I2C_SCL` | `22` | I2C SCL GPIO |
| `DHT_PIN` | `16` | DHT22 data GPIO |
| `TEMT6000_PIN` | `36` | TEMT6000 ADC GPIO |
| `PAIR_BTN_PIN` | `27` | Pairing button GPIO (active-LOW) |
| `LED_PIN` | `5` | WS2812B data GPIO |
| `HW_CONFIG_ID` | `"0x0B"` | Hardware configuration ID embedded in firmware and reported to the gateway for OTA compatibility checks |

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

No LittleFS upload is needed for sensor nodes (no web assets).

After the initial USB flash, future compatible firmware builds can be delivered from the gateway web interface using **Node OTA**.

---

## First Boot & Pairing

1. Flash the firmware
2. On first boot the node prints:
   ```
   [PAIR]  No pairing data - hold button 3 s to pair.
   ```
3. Make sure the gateway is online
4. Hold **GPIO 27** (the pairing button) for **3 seconds** - the LED turns cyan and the node begins beaconing
5. The gateway detects the beacon and completes the handshake automatically
6. The LED turns solid green - the node is now paired and transmitting

At pair time the node sends its sensor schema, firmware version, and hardware configuration ID to the gateway. The gateway and dashboard adapt automatically - no configuration required on the gateway side.

---

## Node OTA Update Support

When the gateway starts a Node OTA update for this device:

1. the node receives an OTA begin request over ESP-NOW
2. it connects to the temporary helper AP created by the gateway's ESP32-C3 coprocessor
3. it downloads the staged `firmware.bin`
4. it finalizes the new image, reboots, and re-registers automatically

Validated behaviors now include:

- same-version reflashing
- version upgrades
- version downgrades

The gateway now validates both the `SENSOR` role marker and the matching `HW_CONFIG_ID` before the OTA helper flow is started.

---

## Sensor Schema

The node reports the following sensors. Only sensors that initialise successfully at boot are included in the schema.

| ID | Label | Unit | Source |
|----|-------|------|--------|
| 0 | Temperature | degC or degF | BMP280 |
| 1 | Atmospheric Pressure | hPa | BMP280 |
| 2 | Humidity | % | DHT22 |
| 3 | Ambient Light | % | TEMT6000 |

The light level is scaled logarithmically to match perceptual brightness - a well-lit room reads approximately 40-60% at default sensitivity.

---

## Per-Node Settings

These settings are configurable from the dashboard (Gateway -> Connected Nodes -> Settings):

| Setting | Type | Default | Range / Options |
|---------|------|---------|-----------------|
| Temp Unit | Enum | degC | degC / degF |
| Send Intvl | Integer | 10 s | 5-60 s (step 5) |
| Status LED | Bool | On | On / Off |
| Light Sens | Integer | 5 | 1-10 (step 1) |

Settings are persisted in NVS under the namespace `"nodeconf"` - separate from pairing data, so a factory reset of the gateway does **not** wipe node settings.

- Changing **Temp Unit** affects the value transmitted; conversion happens on the node before transmission. The sensor schema unit label updates automatically.
- Changing **Send Intvl** takes effect immediately - no reboot required.
- Disabling **Status LED** turns off the WS2812B except during pairing and boot sequences.
- **Light Sens** shifts the entire brightness curve up or down. Lower values make the sensor less responsive (useful in very bright environments); higher values increase sensitivity in dim conditions.

---

## Serial Monitor

```bash
pio device monitor
```

Default baud rate: **115200**. Log prefixes:

| Prefix | Meaning |
|--------|---------|
| `[BOOT]` | Startup sequence |
| `[BMP]` | BMP280 initialisation result |
| `[DHT]` | DHT22 initialisation result |
| `[TEMT]` | TEMT6000 ADC initialisation result |
| `[SENS]` | Sensor schema transmit and per-cycle reading results |
| `[PAIR]` | Pairing handshake steps |
| `[CFG]` | Settings load / save / apply |
| `[NVS]` | NVS read / write |
| `[MESH]` | Gateway-loss detection and re-registration |
| `[ESP-NOW]` | TX result callbacks |
| `[OTA]` | Gateway-managed node OTA download, flash, and reboot flow |

---

## NVS Namespaces

| Namespace | Contents |
|-----------|----------|
| `"mesh"` | Pairing data (gateway MAC, channel, node ID) - cleared by factory reset |
| `"nodeconf"` | Node settings (temp unit, send interval, LED enable, light sensitivity) - **not** cleared by factory reset |
