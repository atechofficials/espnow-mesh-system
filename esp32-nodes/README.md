# ESP32 Nodes

This directory contains firmware for all node types in the ESPNow Mesh System. Nodes communicate over **ESP-NOW** for normal mesh operation and do not need your home Wi-Fi credentials.

During **gateway-managed Node OTA**, the selected node temporarily connects to the helper AP created by the gateway's ESP32-C3 coprocessor, downloads its staged firmware image, flashes it, and then returns to normal ESP-NOW operation after reboot.

---

## Node Types

### Sensor Nodes (`sensor_nodes/`)

Sensor nodes periodically read one or more physical sensors and transmit the data to the gateway. Each node self-describes its sensors to the gateway via a schema handshake at pair time, defining labels, units, and precision for every measurement. The gateway and web dashboard adapt automatically, so adding sensors to a node requires only node firmware changes.

Currently implemented:

| Node | Sensors | Firmware |
|------|---------|----------|
| Envo Mini V1 Node | Bosch BMP280 (temperature + pressure) + DHT22 (humidity) + TEMT6000 (ambient light) | v2.1.1 |

### Actuator Nodes (`actuator_nodes/`)

Actuator nodes receive relay-toggle commands from the gateway and report their current state back.

Currently implemented:

| Node | Actuator | Firmware |
|------|---------|----------|
| ESP32 Relay Node v1 | 4-Relays | v1.1.1 |

---

## How Pairing Works

Every node ships unpaired. To add a node to the mesh:

1. Power on the gateway (must be online and connected to Wi-Fi)
2. Hold the **pairing button** on the node for **3 seconds**; the node enters pairing mode and broadcasts beacon packets on channels 1-13
3. The gateway detects the beacon, displays the node in the **"Available Nodes"** section of the dashboard, and initiates the handshake automatically
4. Within a few seconds the node appears as **Online** in the Connected Nodes table

Pairing data (gateway MAC, channel, assigned node ID) is saved to NVS on the node and survives power cycles.

---

## How Re-registration Works

If the gateway reboots or loses power, nodes detect the absence of acknowledgements and automatically re-send their registration message. The gateway reassigns the same ID (if the node was previously known) and the mesh reassembles without any user intervention.

Current node firmwares also re-report their `HW_CONFIG_ID` during registration and re-registration so the gateway can keep hardware-aware OTA checks available even after restarts.

---

## Gateway-Managed Node OTA

Supported node firmwares can now be updated from the **gateway web interface**.

During Node OTA:

1. the gateway validates the uploaded node `firmware.bin`
2. the ESP32-C3 helper stages that image and starts a temporary OTA AP
3. the selected node joins the helper AP, downloads the image, flashes it, and reboots
4. the node re-registers to the gateway with its existing node ID

This flow has now been validated with:

- sensor-node OTA
- relay-node OTA
- version upgrades
- version downgrades
- same-version reflashing
- role mismatch rejection
- hardware-config mismatch rejection

---

## LED Status Codes (all node types)

| Colour / Pattern | Meaning |
|------------------|---------|
| Fast white flash | Booting |
| Slow cyan pulse | Pairing mode; beaconing |
| Solid green | Paired and transmitting normally |
| Amber slow pulse | Gateway lost; attempting re-registration |
| Off | Status LED disabled via settings |

Pairing-state LEDs (cyan, booting) are always shown regardless of the LED-enable setting.

---

## Shared Protocol

All nodes share `include/mesh_protocol.h` with the gateway. **Keep this file identical** across both projects; a version mismatch will cause silent communication failures.

See the [mesh protocol header](../esp32-gateway/gateway_v1/include/mesh_protocol.h) for the full binary frame layout and message type definitions.
