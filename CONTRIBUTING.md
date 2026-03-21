# Contributing to ESPNow Mesh System

Thank you for contributing to the ESPNow Mesh System.

This project is an ESP32-based mesh platform built around an ESP32-S3 gateway, an ESP32-C3 gateway coprocessor, schema-driven sensor nodes, actuator nodes such as relay controllers, and Hybrid nodes that combine multiple capabilities in one firmware image. Contributions are welcome across firmware, protocol design, dashboard UX, documentation, and hardware support.

---

## Table of Contents

- [Project Status](#project-status)
- [Repository Structure](#repository-structure)
- [Development Environment](#development-environment)
- [Build and Flash Basics](#build-and-flash-basics)
- [Architecture Guidelines](#architecture-guidelines)
- [Ways to Contribute](#ways-to-contribute)
- [Working on the Gateway Firmware](#working-on-the-gateway-firmware)
- [Working on the Gateway Coprocessor Firmware](#working-on-the-gateway-coprocessor-firmware)
- [Working on the Web Interface](#working-on-the-web-interface)
- [Working on Sensor Nodes](#working-on-sensor-nodes)
- [Working on Actuator Nodes](#working-on-actuator-nodes)
- [Working on Hybrid Nodes](#working-on-hybrid-nodes)
- [Changing mesh_protocol.h](#changing-mesh_protocolh)
- [Changing coproc_ota_protocol.h](#changing-coproc_ota_protocolh)
- [Versioning Guidelines](#versioning-guidelines)
- [Pull Request Checklist](#pull-request-checklist)
- [Ground Rules](#ground-rules)

---

## Project Status

Current file versions:

| Component | Current Version |
|----------|-----------------|
| ESP32-S3 Gateway firmware `main.cpp` | `v2.1.1` |
| ESP32-C3 Gateway Coprocessor firmware `main.cpp` | `v0.1.1` |
| ESP32 Sensor Node firmware `main.cpp` | `v2.1.2` |
| ESP32 Actuator Relay Node firmware `main.cpp` | `v1.2.0` |
| ESP32 Hybrid Relay Node firmware `main.cpp` | `v0.1.0` |
| `mesh_protocol.h` | `v3.3.0` |
| `coproc_ota_protocol.h` | `v1.0.0` |
| `index.html` | `v3.8` |
| `app.js` | `v4.2` |
| `style.css` | `v3.7` |

Current supported node categories:
- Sensor nodes
- Actuator nodes
- Hybrid nodes

The current Node OTA system has been validated with:
- same-version reflashing
- firmware upgrades
- firmware downgrades
- sensor-node OTA
- relay-node OTA
- hybrid-node OTA
- node-role mismatch rejection
- node hardware-config mismatch rejection
- gateway hardware-config mismatch rejection
- gateway-image rejection on the node OTA route

---

## Repository Structure

```text
espnow-mesh-system/
|-- README.md
|-- CONTRIBUTING.md
|-- esp32-gateway/
|   |-- README.md
|   `-- gateway_v1/
|       |-- src/main.cpp
|       |-- include/mesh_protocol.h
|       |-- include/coproc_ota_protocol.h
|       |-- coprocessor_esp32c3/
|       |   |-- src/main.cpp
|       |   |-- include/coproc_ota_protocol.h
|       |   `-- platformio.ini
|       |-- data/
|       |   |-- index.html
|       |   |-- js/app.js
|       |   `-- css/style.css
|       |-- partitions_8mb_noot.csv
|       |-- partitions_8mb_ota.csv
|       |-- platformio.ini
|       `-- README.md
`-- esp32-nodes/
    |-- README.md
    |-- sensor_nodes/
    |   |-- README.md
    |   `-- envo_mini_v1/
    |       |-- src/main.cpp
    |       |-- include/mesh_protocol.h
    |       |-- platformio.ini
    |       `-- README.md
    |-- actuator_nodes/
    |   |-- README.md
    |   `-- esp32_relay_node_v1/
    |       |-- src/main.cpp
    |       |-- include/mesh_protocol.h
    |       |-- platformio.ini
    |       `-- README.md
    `-- hybrid_nodes/
        |-- README.md
        `-- esp32_hybrid_relay_node_v1/
            |-- src/main.cpp
            |-- include/mesh_protocol.h
            |-- platformio.ini
            `-- README.md
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

### Gateway coprocessor

From `esp32-gateway/gateway_v1/coprocessor_esp32c3/`:

```bash
pio run --target erase
pio run --target upload
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

### Hybrid node

From the specific hybrid node directory:

```bash
pio run --target erase
pio run --target upload
pio device monitor
```

---

## Architecture Guidelines

This project is built around a few important design principles.

### 1. Shared binary mesh protocol

All node-to-gateway ESP-NOW communication uses packed C structs defined in `mesh_protocol.h`.

This file is the protocol contract and must remain identical across:
- gateway firmware
- sensor node firmware
- actuator node firmware
- hybrid node firmware

The current OTA-safety extension also depends on node registration carrying `hw_config_id`, so contributors must keep that field aligned everywhere the shared protocol is copied.

### 2. Shared gateway-helper OTA transport

The ESP32-S3 gateway and ESP32-C3 helper use `coproc_ota_protocol.h` as their UART transport contract for Node OTA staging, control, and status reporting.

This file must remain identical across:
- `esp32-gateway/gateway_v1/include/coproc_ota_protocol.h`
- `esp32-gateway/gateway_v1/coprocessor_esp32c3/include/coproc_ota_protocol.h`

### 3. Schema-driven system design

Sensor nodes describe themselves dynamically through protocol messages. The gateway and dashboard should not rely on hardcoded sensor names.

Actuator nodes should also report current actuator state so the gateway and Web Interface remain synchronized across:
- manual control
- reboot
- reconnect
- persistence restore

### 4. Clear separation of responsibilities

- Node firmware owns hardware interaction and node-side OTA flashing
- Gateway firmware owns pairing, routing, NVS tracking, browser communication, and Node OTA orchestration
- Gateway coprocessor firmware owns helper AP hosting, staged firmware serving, and helper-side OTA status reporting
- Web assets own rendering and user interaction only

OTA compatibility checks are split deliberately:
- the gateway validates firmware type, firmware markers, and hardware-config compatibility
- the helper only stages and serves firmware that has already passed gateway-side checks
- nodes still own the final download, flash, and reboot flow

### 5. Backward-aware protocol evolution

Protocol changes should be deliberate, documented, and versioned. Silent mismatches between different copies of `mesh_protocol.h` or `coproc_ota_protocol.h` are among the easiest ways to break the system.

---

## Ways to Contribute

Useful contribution areas include:

| Area | Examples |
|------|----------|
| Gateway firmware | pairing reliability, reconnect logic, Node OTA coordination, NVS robustness, diagnostics |
| Gateway coprocessor | helper AP behavior, staged firmware serving, transport resilience, helper diagnostics |
| Web Interface | mobile improvements, cleaner UX, Hybrid node controls, RFID UX, Node OTA feedback, better actuator controls |
| Sensor nodes | new hardware integrations, better power handling, schema-driven readings, node OTA robustness |
| Actuator nodes | relay boards, dimmers, motor control, valve control, node OTA robustness |
| Hybrid nodes | combined actuator/sensor devices, RFID workflows, mixed-capability nodes, node OTA robustness |
| Protocol | new messages, hybrid capability support, compatibility improvements |
| Documentation | setup guides, flash instructions, version notes, troubleshooting |
| Tooling | CI builds, release packaging, flashing helpers |

---

## Working on the Gateway Firmware

Main file:
- `esp32-gateway/gateway_v1/src/main.cpp`

Shared protocols:
- `esp32-gateway/gateway_v1/include/mesh_protocol.h`
- `esp32-gateway/gateway_v1/include/coproc_ota_protocol.h`

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
- Node OTA job scheduling, validation, progress reporting, reconnect detection, and persistence of node hardware-config metadata

Before changing gateway logic:
- confirm whether the change belongs in the gateway, helper, or the node
- keep settings and sensor handling schema-driven
- do not hardcode node-specific behavior unless it is truly gateway-specific
- retest end-to-end Node OTA when changing OTA job timing, status handling, or reconnect logic
- preserve `GWHWCFG:` and `NODEHWCFG:` validation behavior when changing OTA upload parsing or firmware-marker scanning

---

## Working on the Gateway Coprocessor Firmware

Main file:
- `esp32-gateway/gateway_v1/coprocessor_esp32c3/src/main.cpp`

Shared protocol:
- `esp32-gateway/gateway_v1/coprocessor_esp32c3/include/coproc_ota_protocol.h`

Things typically handled in the coprocessor:
- UART command/response handling with the ESP32-S3 gateway
- staging uploaded node firmware
- starting the temporary OTA helper AP
- serving the staged `firmware.bin` to the target node
- reporting helper progress back to the gateway

Before changing helper logic:
- keep the gateway and helper copies of `coproc_ota_protocol.h` synchronized
- test the full S3 -> C3 -> node OTA flow on hardware
- validate helper cleanup after OTA success and abort paths

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
- when changing Node OTA UX, verify staging, reconnect, success, and failure states
- keep explicit OTA error messages visible; do not replace specific mismatch errors with generic busy/rebooting text

When modifying the dashboard:
- keep `index.html`, `app.js`, and `style.css` version notes aligned
- test reconnect behavior
- test live update behavior
- test node reboot scenarios
- test settings changes from the browser
- test Node OTA status transitions

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
- maintain the node-side OTA downloader/finalization path

When adding a new sensor node:
1. Duplicate a similar existing node directory
2. Update `NODE_NAME`
3. Add sensor initialization in `setup()`
4. Extend the sensor schema definition
5. Extend sensor data sending logic
6. Add settings only if needed
7. Preserve the node OTA metadata/descriptor handling
8. Define a unique `HW_CONFIG_ID` for the actual hardware build and preserve the `NODEHWCFG` marker/reporting path
9. Update the node README
10. Update `esp32-nodes/sensor_nodes/README.md`

Goal:
A new sensor node should ideally require no gateway code changes if it follows the current schema-driven model and the existing Node OTA contract.

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
- maintain the node-side OTA downloader/finalization path

Current example:
- `esp32-nodes/actuator_nodes/esp32_relay_node_v1/`

When adding or extending an actuator node:
1. Start from an existing actuator-node implementation if possible
2. Keep actuator state reporting reliable
3. Test persistence behavior carefully
4. Test reboot and reconnect state sync
5. Preserve the node OTA metadata/descriptor handling
6. Define a unique `HW_CONFIG_ID` for the actual hardware build and preserve the `NODEHWCFG` marker/reporting path
7. Document every exposed setting in that node's README
8. Update `esp32-nodes/actuator_nodes/README.md`

Important:
Actuator nodes should never assume the Web Interface can guess real hardware state. The node must report it.

---

## Working on Hybrid Nodes

Hybrid nodes combine multiple capability families in one firmware image. The first production example is:

- `esp32-nodes/hybrid_nodes/esp32_hybrid_relay_node_v1/`

Typical hybrid-node responsibilities:
- report the correct `NODE_HYBRID` role and capability flags during registration
- expose actuator schema and current actuator state reliably
- expose any hybrid-specific configuration such as RFID card mappings
- keep auxiliary hardware features from breaking the existing pairing, reboot, persistence, or OTA flows
- maintain the node-side OTA downloader/finalization path

When adding or extending a hybrid node:
1. Start from the closest existing actuator or sensor implementation
2. Keep node capabilities explicit and aligned with `mesh_protocol.h`
3. Preserve actuator-state reporting so the gateway and dashboard stay synchronized
4. Test the mixed-capability Web Interface paths on real hardware
5. Preserve the node OTA metadata/descriptor handling
6. Define a unique `HW_CONFIG_ID` for the actual hardware build and preserve the `NODEHWCFG` marker/reporting path
7. Document every exposed setting and hybrid-specific workflow in that node's README
8. Update `esp32-nodes/hybrid_nodes/README.md`

Important:
Hybrid nodes should be capability-driven, not hardcoded in the gateway or dashboard by one board name.

---

## Changing mesh_protocol.h

Current version: `v3.3.0`

File locations must stay synchronized:
- `esp32-gateway/gateway_v1/include/mesh_protocol.h`
- every node project's `include/mesh_protocol.h`

When changing the protocol:
- document every added message type clearly
- document every struct field clearly
- keep packing assumptions consistent
- update all copies immediately
- validate both gateway and node builds after the change
- re-check any OTA metadata that depends on registration contents, including `hw_config_id`

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

## Changing coproc_ota_protocol.h

Current version: `v1.0.0`

File locations must stay synchronized:
- `esp32-gateway/gateway_v1/include/coproc_ota_protocol.h`
- `esp32-gateway/gateway_v1/coprocessor_esp32c3/include/coproc_ota_protocol.h`

When changing the helper transport:
- document every added command, ACK, and status field
- keep frame sizes, chunk sizing, and CRC assumptions aligned
- update both copies immediately
- validate both gateway and coprocessor builds after the change
- run at least one real Node OTA test on hardware

Use these versioning rules:
- patch bump for comments or non-functional cleanup
- minor bump for additive, backward-compatible helper transport extensions
- major bump for incompatible framing or behavior changes

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
- Node OTA orchestration
- gateway-side OTA safety validation such as hardware-config checks

### Gateway coprocessor firmware

Bump when helper behavior changes:
- UART transport handling
- staged firmware serving
- helper AP behavior
- helper status reporting
- cleanup and abort handling

### Node firmware

Bump when node behavior changes:
- hardware handling
- settings
- state persistence
- reconnect behavior
- schema definitions
- message handling
- Node OTA downloader/finalization behavior
- hardware identity reporting used by gateway-managed OTA

### Web assets

Bump file versions when behavior or layout changes:
- `index.html`
- `app.js`
- `style.css`

### Protocols

Bump `mesh_protocol.h` when the node/gateway communication contract changes.

Bump `coproc_ota_protocol.h` when the gateway/helper transport contract changes.

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
- keep `coproc_ota_protocol.h` synchronized everywhere it exists
- mention any required flash steps such as `uploadfs`
- describe whether the change affects:
  - gateway firmware
  - gateway coprocessor firmware
  - web assets
  - sensor nodes
  - actuator nodes
  - hybrid nodes
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
- Do not let copies of `coproc_ota_protocol.h` drift apart
- Keep `HW_CONFIG_ID`, `GWHWCFG`, and `NODEHWCFG` behavior aligned with the actual hardware variant being built
- Prefer schema-driven design over hardcoded gateway/dashboard assumptions
- Keep contributions focused and easy to review
- Test actuator-state sync after reboot and reconnect whenever actuator logic changes
- Test NVS-backed settings and persistence whenever settings logic changes
- Test end-to-end Node OTA when changing gateway OTA orchestration, helper transport, or node OTA logic
- If you change `data/`, remember `uploadfs`
- Update documentation when user-visible behavior changes
- Do not merge protocol changes without checking all affected firmware targets

---

Thank you for helping improve the ESPNow Mesh System.
