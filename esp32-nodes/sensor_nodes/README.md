# Sensor Nodes

Sensor nodes read physical measurements and transmit them to the gateway over ESP-NOW every configurable interval (default: every 10 seconds).

Supported sensor-node firmwares can also be updated through the **gateway-managed Node OTA** flow. During that OTA window, the selected node temporarily joins the helper AP created by the gateway's ESP32-C3 coprocessor, downloads the staged firmware, flashes it, and then returns to normal ESP-NOW operation after reboot.

As of the current mesh protocol (`v3.3.1`), nodes are **self-describing**. Each node sends a sensor schema to the gateway at pair time that defines the labels, units, and precision of its readings. The gateway and web dashboard have no hardcoded knowledge of any specific sensor type and adapt automatically. Adding a new sensor to a node still requires only node firmware changes when the schema contract is preserved.

Current sensor-node firmwares also report a hardware configuration ID during registration so the gateway can block incompatible Node OTA images before delivery starts. They continue to work unchanged with the newer Hybrid-capable gateway/dashboard release.

---

## Available Sensor Nodes

| Directory | Sensors | Measurements | Board |
|-----------|---------|--------------|-------|
| `envo_mini_v1/` | Bosch BMP280 + DHT22 + TEMT6000 | Temperature, Atmospheric Pressure, Humidity, Ambient Light | DFRobot Firebeetle 2 ESP32-E (`v2.1.4`) |

---

## Adding a New Sensor Node

1. Copy an existing sensor node directory as a starting point
2. Add your sensor driver and initialisation logic in `setup()`
3. Add a `SensorDef` entry for each new measurement in `getSensorDefs()` and define its `id`, `label`, `unit`, and `precision`
4. Add the corresponding reading in `sendSensorData()` using the same `id`
5. Update `NODE_NAME` at the top of `main.cpp` (fresh, unrenamed nodes append the last 4 MAC characters automatically, so keep the base name within the current 24 visible-character limit)
6. Add per-node settings in `loadSettings()` / `saveSettings()` / `getSettingsDefs()` if needed; the gateway displays them automatically in the node settings panel
7. Preserve the node OTA metadata, role/version information, and `HW_CONFIG_ID` marker/reporting used by the gateway when validating uploaded node firmware images

No changes to `mesh_protocol.h`, the gateway firmware, or the web interface are required. The gateway learns what sensors a node has from the schema handshake at pair time.

The pairing flow, heartbeat, gateway-loss recovery, settings protocol, schema handshake, and Node OTA downloader flow are consistent across node types and can be reused from existing implementations.
