# Actuator Nodes

> **Coming soon.** This directory will contain firmware for relay and other actuator node types.

---

## Planned Node Types

| Node | Actuator | Description |
|------|----------|-------------|
| `relay_node/` | 4-channel relay board | Toggle up to 4 relays independently from the dashboard |

---

## Design Notes

Actuator nodes will use the same pairing flow, heartbeat, and settings protocol as sensor nodes. The gateway already handles `MSG_RELAY_CMD` and `MSG_RELAY_STATE` message types — the firmware just needs to be written.

To implement a relay node, the key tasks are:

1. Handle `MSG_RELAY_CMD` — apply the relay bitmask to the GPIO outputs
2. Broadcast `MSG_RELAY_STATE` — send the current bitmask back to the gateway after each change and periodically as a heartbeat payload
3. Optionally expose per-node settings (e.g. default relay state on power-up) using the existing settings schema protocol