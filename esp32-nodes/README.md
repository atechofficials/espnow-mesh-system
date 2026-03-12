# ESP32 Nodes

This directory contains firmware for all node types in the ESPNow Mesh System. Nodes are ESP32 devices that communicate exclusively over **ESP-NOW** — they never connect to Wi-Fi directly.

---

## Node Types

### Sensor Nodes (`sensor_nodes/`)

Sensor nodes periodically read one or more physical sensors and transmit the data to the gateway. They also respond to settings requests (temperature unit, send interval, LED behaviour, etc.).

Currently implemented:

| Node | Sensor | Firmware |
|------|--------|----------|
| Envo Mini V1 Node | Bosch BMP280 (temperature + pressure) | v1.3.0 |

### Actuator Nodes (`actuator_nodes/`)

Actuator nodes receive relay-toggle commands from the gateway and report their current state back. Planned for a future release.

---

## How Pairing Works

Every node ships unpaired. To add a node to the mesh:

1. Power on the gateway (must be online and connected to Wi-Fi)
2. Hold the **pairing button** on the node for **3 seconds** — the node enters pairing mode and broadcasts beacon packets on channels 1–13
3. The gateway detects the beacon, displays the node in the **"Available Nodes"** section of the dashboard, and initiates the handshake automatically
4. Within a few seconds the node appears as **Online** in the Connected Nodes table

Pairing data (gateway MAC, channel, assigned node ID) is saved to NVS on the node and survives power cycles.

---

## How Re-registration Works

If the gateway reboots or loses power, nodes detect the absence of acknowledgements and automatically re-send their registration message. The gateway reassigns the same ID (if the node was previously known) and the mesh reassembles without any user intervention.

---

## LED Status Codes (all node types)

| Colour / Pattern | Meaning |
|------------------|---------|
| Fast white flash | Booting |
| Slow cyan pulse | Pairing mode — beaconing |
| Solid green | Paired and transmitting normally |
| Amber slow pulse | Gateway lost — attempting re-registration |
| Off | Status LED disabled via settings |

Pairing-state LEDs (cyan, booting) are always shown regardless of the LED-enable setting.

---

## Shared Protocol

All nodes share `include/mesh_protocol.h` with the gateway. **Keep this file identical** across both projects — a version mismatch will cause silent communication failures.

See the [mesh protocol header](../esp32-gateway/gateway_v1/include/mesh_protocol.h) for the full binary frame layout and message type definitions.