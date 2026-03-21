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
- automatic re-registration after reboot or gateway restart
- gateway-managed Node OTA with role and hardware-config validation

---

## Design Notes

Hybrid nodes should be treated as capability-driven devices, not hardcoded one-off exceptions. The gateway and web dashboard now use the node's reported type and capabilities to decide whether to show:

- sensor panels
- actuator controls
- Hybrid-specific sections such as RFID card actions

This keeps the first Hybrid implementation simple while leaving room for future nodes that combine sensors, actuators, and other hardware features in one firmware image.

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
