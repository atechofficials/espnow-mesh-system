# Actuator Nodes

This directory contains firmware for ESP32-based actuator nodes used in the ESP-NOW Mesh System.

Actuator nodes receive commands from the gateway and control physical outputs such as relays. They follow the same pairing, heartbeat, registration, and settings flow used by sensor nodes, while also reporting actuator state back to the gateway so the Web Interface stays synchronized.

Supported actuator-node firmwares can also be updated through the **gateway-managed Node OTA** flow. During that OTA window, the selected node temporarily joins the helper AP created by the gateway's ESP32-C3 coprocessor, downloads the staged firmware, flashes it, and then returns to normal ESP-NOW operation after reboot.

---

## Currently Available

| Node | Actuator Type | Description |
|------|---------------|-------------|
| `esp32_relay_node_v1/` | 4-channel relay control + TTP224 Touch Sensor | Controls up to 4 active-LOW relays independently from the Web Interface |

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

---

## Protocol Notes

Actuator nodes use the shared `mesh_protocol.h` definitions and the actuator messaging available in protocol version **v3.2.0**.

### Actuator-related message types

- `MSG_ACTUATOR_SCHEMA_GET`
- `MSG_ACTUATOR_SCHEMA`
- `MSG_ACTUATOR_STATE`
- `MSG_ACTUATOR_SET`

### Node types

- `NODE_SENSOR`
- `NODE_ACTUATOR`
- `NODE_HYBRID`
  Reserved for future development of nodes that combine both sensing and actuation in one device.

---

## Design Overview

Actuator nodes are designed to work much like sensor nodes, but instead of mainly publishing readings, they also:

1. Accept actuator commands from the gateway
2. Apply those commands to physical outputs
3. Report the resulting actuator state back to the gateway
4. Expose configurable node settings where needed
5. Participate in the gateway-managed Node OTA workflow when a compatible firmware image is uploaded from the dashboard

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
- supports gateway-managed Node OTA

---

## Future Expansion

This directory is intended to grow with additional actuator node types, for example:

- relay boards with more channels
- dimmer nodes
- motor control nodes
- valve/solenoid controller nodes
- hybrid sensor + actuator nodes

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
