# Sensor Nodes

Sensor nodes read physical measurements and transmit them to the gateway over ESP-NOW every configurable interval (default: every 10 seconds).

---

## Available Sensor Nodes

| Directory | Sensor | Measurements | Board |
|-----------|--------|--------------|-------|
| `envo_mini_v1/` | Bosch BMP280 | Temperature, Pressure | DFRobot Firebeetle 2 ESP32-E |

---

## Adding a New Sensor Node

1. Copy an existing sensor node directory as a starting point
2. Replace the sensor driver and `sendSensorData()` function with your sensor's logic
3. Update `MSG_SENSOR_DATA` in `mesh_protocol.h` if you need to transmit different data fields, **and** update the gateway's handler for `MSG_SENSOR_DATA` to parse the new struct
4. Update `NODE_NAME` at the top of `main.cpp`
5. Add per-node settings in `loadSettings()` / `saveSettings()` / `getSettingsDefs()` if needed — the gateway will display them automatically in the node settings panel

The pairing flow, heartbeat, gateway-loss recovery, and settings protocol are identical across all sensor node types and can be copied verbatim.