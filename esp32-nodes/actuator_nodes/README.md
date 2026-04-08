# Actuator Nodes

This directory contains firmware for ESP32-based actuator nodes used in the ESP-NOW Mesh System.

Actuator nodes receive commands from the gateway and control physical outputs such as relays. They follow the same pairing, heartbeat, registration, and settings flow used by sensor nodes, while also reporting actuator state back to the gateway so the Web Interface stays synchronized.

Supported actuator-node firmwares can also be updated through the **gateway-managed Node OTA** flow. During that OTA window, the selected node temporarily joins the helper AP created by the gateway's ESP32-C3 coprocessor, downloads the staged firmware, flashes it, and then returns to normal ESP-NOW operation after reboot.

---

## Currently Available

| Node | Actuator Type | Description |
|------|---------------|-------------|
| `esp32_relay_node_v1/` | 4-channel relay control + TTP224 Touch Sensor | Controls up to 4 active-LOW relays independently from the Web Interface (`v1.3.1`) |

---

## Supported Features

- ESP-NOW pairing with the gateway
- Automatic node registration and re-registration after reboot
- Heartbeat reporting
- Actuator state reporting back to the gateway
- Dashboard-based actuator control
- Per-node settings exposed in the Web Interface
- Persistent settings stored in NVS
- Optional relay state persistence, depending on node firmware
- Gateway-managed Node OTA delivery for supported actuator-node firmware
- Hardware-config-aware OTA safety checks for compatible actuator-node firmware

---

## Protocol Notes

Actuator nodes use the shared `mesh_protocol.h` definitions and the actuator messaging available in protocol version **v3.3.2**.

### Actuator-related message types

- `MSG_ACTUATOR_SCHEMA_GET`
- `MSG_ACTUATOR_SCHEMA`
- `MSG_ACTUATOR_STATE`
- `MSG_ACTUATOR_SET`

### Node types

- `NODE_SENSOR`
- `NODE_ACTUATOR`
- `NODE_HYBRID`
  Used by Hybrid-node firmwares that combine capabilities such as actuators plus RFID or sensors plus actuators in one device.

---

## Design Overview

Actuator nodes are designed to work much like sensor nodes, but instead of mainly publishing readings, they also:

1. Accept actuator commands from the gateway
2. Apply those commands to physical outputs
3. Report the resulting actuator state back to the gateway
4. Expose configurable node settings where needed
5. Participate in the gateway-managed Node OTA workflow when a compatible firmware image is uploaded from the dashboard

Current actuator-node firmwares also report their `HW_CONFIG_ID` during registration so the gateway can block incompatible actuator firmware before OTA delivery begins.

The newer release line also moves user-tunable actuator-node definitions into `user_config.h`. If a future actuator-node board uses the ESP32-C3 Super Mini, keep any WiFi transmit power cap board-gated and apply it only after Wi-Fi startup.

The current `v1.3.1` actuator-node line also tightens heartbeat/liveness timing so the node feels more stable and responsive with the gateway and with the gateway's MQTT/Home Assistant bridge.

When MQTT is enabled on the gateway:

- relay controls and supported actuator-node settings are exposed through Home Assistant MQTT discovery
- actuator state stays synchronized between Home Assistant and the gateway dashboard
- relay label settings remain dashboard-only, because Home Assistant already provides local naming for switches and buttons

Hybrid nodes that also expose actuator behavior are now documented separately under `../hybrid_nodes/`, but they share the same gateway-side actuator schema/state handling model.

This keeps the gateway and Web Interface synchronized after:
- actuator toggles
- node reboot
- gateway reconnect
- state persistence restore
- node OTA reboot/rejoin

---

## Current Implementation

### `esp32_relay_node_v1`

The first actuator firmware implementation is a relay control node.

Main capabilities:
- controls 4 relays independently
- reports relay states back to the gateway
- supports relay state persistence across reboot
- supports Status LED enable/disable setting
- exposes settings in the gateway Web Interface for easier control
- supports per-relay labels set from the Web Interface
- supports gateway-managed Node OTA with role and hardware-config validation

---

## Future Expansion

This directory is intended to grow with additional actuator node types, for example:

- relay boards with more channels
- dimmer nodes
- motor control nodes
- valve/solenoid controller nodes
- higher-channel relay nodes

Hybrid sensor + actuator boards now have a dedicated sibling directory:

- `../hybrid_nodes/`

---

## Directory Structure

```text
actuator_nodes/
|-- README.md
`-- esp32_relay_node_v1/
    |-- README.md
    |-- src/
    |-- include/
    |-- lib/
    `-- platformio.ini
```

For board-specific wiring, firmware configuration, build steps, settings, and Node OTA behavior, see the README inside each actuator node folder.
