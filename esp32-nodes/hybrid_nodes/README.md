# Hybrid Nodes

Hybrid nodes combine multiple capability families in one firmware image. In the current project release, that means a node can participate in the actuator flow, expose Hybrid-specific configuration, and still be handled cleanly by the gateway and dashboard as a first-class `NODE_HYBRID` device.

Like the other node families, Hybrid nodes use ESP-NOW for normal mesh traffic and the gateway-managed helper-AP flow for **Node OTA** updates.

---

## Currently Available

| Node | Capabilities | Description |
|------|--------------|-------------|
| `esp32_hybrid_relay_node_v1/` | 4 relays + 4 touch inputs + RC522 RFID | Hybrid relay controller with RFID card actions and dashboard-managed relay scenes |

---

## Supported Features

- capability-based registration with `NODE_HYBRID`
- actuator schema reporting and live actuator-state sync
- Hybrid-specific configuration exchange for RFID card actions
- RFID scan events sent to the gateway/dashboard for scan-to-learn UX
- periodic RC522 health checks with automatic reader reinitialization if the RFID interface stalls after long uptime
- automatic re-registration after reboot or gateway restart
- gateway-managed Node OTA with role and hardware-config validation

---

## Design Notes

Hybrid nodes should be treated as capability-driven devices, not hardcoded one-off exceptions. The gateway and web dashboard now use the node's reported type and capabilities to decide whether to show:

- sensor panels
- actuator controls
- Hybrid-specific sections such as RFID card actions

This keeps the first Hybrid implementation simple while leaving room for future nodes that combine sensors, actuators, and other hardware features in one firmware image.

For custom ESP32 hardware, Hybrid-node peripheral wiring should also avoid unnecessary use of ESP32 boot-strapping pins when safer GPIOs are available. The current RC522-based Hybrid reference now uses a non-strapping GPIO for the reader reset line so USB flashing remains reliable with the RFID module connected.

---

## Directory Structure

```text
hybrid_nodes/
|-- README.md
`-- esp32_hybrid_relay_node_v1/
    |-- README.md
    |-- src/
    |-- include/
    |-- lib/
    `-- platformio.ini
```

For board-specific wiring, firmware configuration, build steps, settings, and RFID behavior, see the README inside each Hybrid node folder.
