# Contributing to ESPNow Mesh System

Thank you for contributing to the ESPNow Mesh System.

This project is an ESP32-based mesh platform built around an ESP32-S3 gateway, schema-driven sensor nodes, and actuator nodes such as relay controllers. Contributions are welcome across firmware, protocol design, dashboard UX, documentation, and hardware support.

---

## Table of Contents

- [Project Status](#project-status)
- [Repository Structure](#repository-structure)
- [Development Environment](#development-environment)
- [Build and Flash Basics](#build-and-flash-basics)
- [Architecture Guidelines](#architecture-guidelines)
- [Ways to Contribute](#ways-to-contribute)
- [Working on the Gateway Firmware](#working-on-the-gateway-firmware)
- [Working on the Web Interface](#working-on-the-web-interface)
- [Working on Sensor Nodes](#working-on-sensor-nodes)
- [Working on Actuator Nodes](#working-on-actuator-nodes)
- [Changing mesh_protocol.h](#changing-mesh_protocolh)
- [Versioning Guidelines](#versioning-guidelines)
- [Pull Request Checklist](#pull-request-checklist)
- [Ground Rules](#ground-rules)

---

## Project Status

Current file versions:

| Component | Current Version |
|----------|-----------------|
| ESP32-S3 Gateway firmware `main.cpp` | `v1.8.3` |
| ESP32 Sensor Node firmware `main.cpp` | `v2.0.1` |
| ESP32 Actuator Relay Node firmware `main.cpp` | `v1.0.1` |
| `mesh_protocol.h` | `v3.1.0` |
| `index.html` | `v3.5` |
| `app.js` | `v3.7` |
| `style.css` | `v3.4` |

Current supported node categories:
- Sensor nodes
- Actuator nodes
- Hybrid nodes are reserved in the protocol for future development

---

## Repository Structure

```text
espnow-mesh-system/
├── README.md
├── CONTRIBUTING.md
├── esp32-gateway/
│   ├── README.md
│   └── gateway_v1/
│       ├── src/main.cpp
│       ├── include/mesh_protocol.h
│       ├── data/
│       │   ├── index.html
│       │   ├── js/app.js
│       │   └── css/style.css
│       ├── platformio.ini
│       └── README.md
└── esp32-nodes/
    ├── README.md
    ├── sensor_nodes/
    │   ├── README.md
    │   └── envo_mini_v1/
    │       ├── src/main.cpp
    │       ├── include/mesh_protocol.h
    │       ├── platformio.ini
    │       └── README.md
    └── actuator_nodes/
        ├── README.md
        └── esp32_relay_node_v1/
            ├── src/main.cpp
            ├── include/mesh_protocol.h
            ├── platformio.ini
            └── README.md
```

---

## Development Environment

Requirements:

- PlatformIO Core or the PlatformIO IDE extension for VS Code
- One USB data cable per ESP32 device being flashed or monitored
- Real hardware for validation

PlatformIO manages:
- ESP32 toolchains
- framework packages
- library dependencies
- board configuration

No manual toolchain setup should be required beyond installing PlatformIO.

---

## Build and Flash Basics

### Gateway

From `esp32-gateway/gateway_v1/`:

```bash
# Optional clean erase
pio run --target erase

# Upload web assets when anything inside data/ changes
pio run --target uploadfs

# Build and flash firmware
pio run --target upload

# Serial monitor
pio device monitor
```

### Sensor node

From the specific sensor node directory:

```bash
pio run --target erase
pio run --target upload
pio device monitor
```

### Actuator node

From the specific actuator node directory:

```bash
pio run --target erase
pio run --target upload
pio device monitor
```

---

## Architecture Guidelines

This project is built around a few important design principles.

### 1. Shared binary protocol

All node-to-gateway ESP-NOW communication uses packed C structs defined in `mesh_protocol.h`.

This file is the protocol contract and must remain identical across:
- gateway firmware
- sensor node firmware
- actuator node firmware

### 2. Schema-driven system design

Sensor nodes describe themselves dynamically through protocol messages. The gateway and dashboard should not rely on hardcoded sensor names.

Actuator nodes should also report current actuator state so the gateway and Web Interface remain synchronized across:
- manual control
- reboot
- reconnect
- persistence restore

### 3. Clear separation of responsibilities

- Node firmware owns hardware interaction
- Gateway firmware owns pairing, routing, NVS tracking, and browser communication
- Web assets own rendering and user interaction only

### 4. Backward-aware protocol evolution

Protocol changes should be deliberate, documented, and versioned. Silent mismatches between different copies of `mesh_protocol.h` are one of the easiest ways to break the system.

---

## Ways to Contribute

Useful contribution areas include:

| Area | Examples |
|------|----------|
| Gateway firmware | pairing reliability, actuator support, reconnect logic, NVS robustness, diagnostics |
| Web Interface | mobile improvements, cleaner UX, node status views, settings UX, better actuator controls |
| Sensor nodes | new hardware integrations, better power handling, more schema-driven readings |
| Actuator nodes | relay boards, dimmers, motor control, valve control, hybrid nodes |
| Protocol | new messages, future hybrid support, compatibility improvements |
| Documentation | setup guides, flash instructions, version notes, troubleshooting |
| Tooling | CI builds, release packaging, flashing helpers |

---

## Working on the Gateway Firmware

Main file:
- `esp32-gateway/gateway_v1/src/main.cpp`

Shared protocol:
- `esp32-gateway/gateway_v1/include/mesh_protocol.h`

Things typically handled in the gateway:
- Wi-Fi and captive portal setup
- ESP-NOW peer management
- node registration and re-registration
- settings routing
- sensor data handling
- actuator state caching
- WebSocket communication
- HTTP API endpoints
- persistence of gateway-side state

Before changing gateway logic:
- confirm whether the change belongs in the gateway or the node
- keep settings and sensor handling schema-driven
- do not hardcode node-specific behavior unless it is truly gateway-specific

---

## Working on the Web Interface

Main files:
- `esp32-gateway/gateway_v1/data/index.html`
- `esp32-gateway/gateway_v1/data/js/app.js`
- `esp32-gateway/gateway_v1/data/css/style.css`

There is no separate web build system. The dashboard is served directly from LittleFS.

Important notes:
- if you change anything inside `data/`, you must upload LittleFS again
- UI state should reflect the gateway's live state, not invent its own source of truth
- actuator buttons, settings panels, and node status should stay in sync with WebSocket updates
- preserve schema-driven rendering wherever possible

When modifying the dashboard:
- keep `index.html`, `app.js`, and `style.css` version notes aligned
- test reconnect behavior
- test live update behavior
- test node reboot scenarios
- test settings changes from the browser

---

## Working on Sensor Nodes

Sensor nodes should remain as self-describing as possible.

Typical work:
- initialize sensor hardware
- define sensor schema
- send sensor readings
- define per-node settings if needed
- handle gateway settings changes
- keep units, labels, and precision inside node firmware

When adding a new sensor node:
1. Duplicate a similar existing node directory
2. Update `NODE_NAME`
3. Add sensor initialization in `setup()`
4. Extend the sensor schema definition
5. Extend sensor data sending logic
6. Add settings only if needed
7. Update the node README
8. Update `esp32-nodes/sensor_nodes/README.md`

Goal:
A new sensor node should ideally require no gateway code changes if it follows the current schema-driven model.

---

## Working on Actuator Nodes

Actuator nodes are first-class citizens in the current version of the system.

Typical actuator-node responsibilities:
- receive `MSG_ACTUATOR_SET`
- apply actuator output to hardware
- send `MSG_ACTUATOR_STATE`
- expose configurable settings where useful
- restore saved state when persistence is enabled
- resynchronize actuator state after reconnect or reboot

Current example:
- `esp32-nodes/actuator_nodes/esp32_relay_node_v1/`

When adding or extending an actuator node:
1. Start from an existing actuator-node implementation if possible
2. Keep actuator state reporting reliable
3. Test persistence behavior carefully
4. Test reboot and reconnect state sync
5. Document every exposed setting in that node's README
6. Update `esp32-nodes/actuator_nodes/README.md`

Important:
Actuator nodes should never assume the Web Interface can guess real hardware state. The node must report it.

---

## Changing mesh_protocol.h

Current version: `v3.1.0`

File locations must stay synchronized:
- `esp32-gateway/gateway_v1/include/mesh_protocol.h`
- every node project's `include/mesh_protocol.h`

When changing the protocol:
- document every added message type clearly
- document every struct field clearly
- keep packing assumptions consistent
- update all copies immediately
- validate both gateway and node builds after the change

Use these versioning rules:
- patch bump for comments or non-functional cleanup
- minor bump for additive, backward-compatible protocol extensions
- major bump for breaking struct layout or incompatible behavior changes

Examples of changes that likely require a major bump:
- changing struct sizes
- changing field order
- changing meanings of existing message IDs
- removing or repurposing existing node/message types

---

## Versioning Guidelines

Keep version notes consistent across the project.

### Gateway firmware
Bump when gateway logic changes in a meaningful way:
- pairing behavior
- routing logic
- settings handling
- actuator support
- state sync
- HTTP/WebSocket behavior

### Node firmware
Bump when node behavior changes:
- hardware handling
- settings
- state persistence
- reconnect behavior
- schema definitions
- message handling

### Web assets
Bump file versions when behavior or layout changes:
- `index.html`
- `app.js`
- `style.css`

### Protocol
Bump `mesh_protocol.h` only when the communication contract changes.

If a release spans multiple components, update changelog entries in the affected README files as well.

---

## Pull Request Checklist

Before opening a PR:

- build the affected project successfully
- flash and test on real hardware when behavior changes
- test serial monitor logs
- update the correct README files
- keep version references current
- keep `mesh_protocol.h` synchronized everywhere
- mention any required flash steps such as `uploadfs`
- describe whether the change affects:
  - gateway firmware
  - web assets
  - sensor nodes
  - actuator nodes
  - protocol compatibility

Recommended PR structure:
- summary of what changed
- why it changed
- files/components affected
- hardware tested
- logs or screenshots if useful
- any protocol/version bump notes

---

## Ground Rules

- Do not let copies of `mesh_protocol.h` drift apart
- Prefer schema-driven design over hardcoded gateway/dashboard assumptions
- Keep contributions focused and easy to review
- Test actuator-state sync after reboot and reconnect whenever actuator logic changes
- Test NVS-backed settings and persistence whenever settings logic changes
- If you change `data/`, remember `uploadfs`
- Update documentation when user-visible behavior changes
- Do not merge protocol changes without checking all firmware targets

---

Thank you for helping improve the ESPNow Mesh System.
```
