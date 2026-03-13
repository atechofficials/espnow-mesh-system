# Sensor Nodes

Sensor nodes read physical measurements and transmit them to the gateway over ESP-NOW every configurable interval (default: every 10 seconds).

As of protocol v3.0, nodes are **self-describing** — each node sends a sensor schema to the gateway at pair time that defines the labels, units, and precision of its readings. The gateway and web dashboard have no hardcoded knowledge of any specific sensor type and adapt automatically. Adding a new sensor to a node requires only node firmware changes.

---

## Available Sensor Nodes

| Directory | Sensors | Measurements | Board |
|-----------|---------|--------------|-------|
| `envo_mini_v1/` | Bosch BMP280 + DHT22 + TEMT6000 | Temperature, Atmospheric Pressure, Humidity, Ambient Light | DFRobot Firebeetle 2 ESP32-E |

---

## Adding a New Sensor Node

1. Copy an existing sensor node directory as a starting point
2. Add your sensor driver and initialisation logic in `setup()`
3. Add a `SensorDef` entry for each new measurement in `getSensorDefs()` — define its `id`, `label`, `unit`, and `precision`
4. Add the corresponding reading in `sendSensorData()` using the same `id`
5. Update `NODE_NAME` at the top of `main.cpp`
6. Add per-node settings in `loadSettings()` / `saveSettings()` / `getSettingsDefs()` if needed — the gateway displays them automatically in the node settings panel

No changes to `mesh_protocol.h`, the gateway firmware, or the web interface are required. The gateway learns what sensors a node has from the schema handshake at pair time.

The pairing flow, heartbeat, gateway-loss recovery, settings protocol, and schema handshake are identical across all node types and can be copied verbatim from any existing node.