# ESP32-C3 Gateway Coprocessor

Firmware version: **0.1.1**
Target board: `dfrobot_beetle_esp32c3`

This firmware runs on the **ESP32-C3 coprocessor** attached to the ESP32-S3 gateway. It is not a standalone mesh node. Its main job is to help the gateway perform **Node OTA updates** by staging node firmware, creating a temporary helper access point, serving the staged `firmware.bin` over HTTP, and sending helper status back to the gateway over UART.

The helper does not decide firmware compatibility by itself. The ESP32-S3 gateway now validates node role and hardware-config compatibility before the helper staging flow is allowed to begin.

Current helper releases are used for sensor-node, actuator-node, and hybrid-node OTA jobs. The `v0.1.1` line keeps the helper aligned with the Hybrid-node-capable gateway release and improves the helper handshake/readiness flow used by the ESP32-S3 gateway.

---

## What This Coprocessor Does

During a Node OTA update, the helper:

1. receives OTA commands from the ESP32-S3 gateway over UART
2. stages the uploaded node `firmware.bin` into its local filesystem
3. starts a temporary Wi-Fi access point for the target node
4. serves the staged firmware at `http://192.168.4.1/firmware.bin`
5. accepts helper status reports from the node on `/status`
6. reports progress and errors back to the gateway

The gateway remains the OTA orchestrator. The ESP32-C3 only handles the helper-side transport and temporary OTA hosting.

This means incompatible uploads such as wrong-role firmware, wrong hardware-config firmware, or gateway firmware accidentally sent through the node OTA path are rejected by the gateway before the helper is engaged.

---

## Hardware

| Item | Detail |
|------|--------|
| Board | DFRobot Beetle ESP32-C3 |
| Firmware role | Gateway coprocessor / Node OTA helper |
| UART link to gateway | `RX=GPIO0`, `TX=GPIO1` |
| UART baud | `230400` |
| Reboot signal pin | `GPIO7` |
| Helper AP IP | `192.168.4.1` |
| Helper HTTP port | `80` |

---

## Shared Protocol

The helper communicates with the ESP32-S3 gateway using:

- `include/coproc_ota_protocol.h`

This file must stay synchronized with:

- `../include/coproc_ota_protocol.h`

Current shared helper transport version:

- `coproc_ota_protocol.h` **v1.0.0**

Important frame types include:

- `COPROC_FRAME_HELLO`
- `COPROC_FRAME_UPLOAD_BEGIN`
- `COPROC_FRAME_UPLOAD_CHUNK`
- `COPROC_FRAME_UPLOAD_END`
- `COPROC_FRAME_ABORT`
- `COPROC_FRAME_STATUS`

---

## Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

| Library | Version |
|---------|---------|
| ArduinoJson | ^7.4.2 |

Framework libraries used directly: `WiFi`, `WebServer`, `LittleFS`.

---

## Build & Flash

From the `coprocessor_esp32c3/` directory:

```bash
# Optional clean erase
pio run --target erase

# Build and upload firmware
pio run --target upload

# Serial monitor
pio device monitor
```

Default serial monitor baud rate: **115200**.

You normally flash this helper once during gateway setup, then update it again only when helper-side behavior or the gateway-helper transport changes.

---

## Runtime Behavior

### Idle state

When no Node OTA job is active, the helper stays connected to the ESP32-S3 gateway over UART and responds to `HELLO` frames with:

- `helper-ready`

### During Node OTA

The helper:

- receives the node firmware in chunks from the gateway
- writes the image to LittleFS as `/firmware.bin`
- starts the temporary OTA helper AP
- serves the firmware to the target node over HTTP
- forwards node progress and completion status back to the gateway

### After completion or abort

The gateway sends an abort/cleanup command and the helper:

- stops the temporary AP
- stops the HTTP server
- returns to the idle `helper-ready` state

---

## HTTP Endpoints

These endpoints are intended for the OTA target node during a live Node OTA session:

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/firmware.bin` | `GET` | Streams the staged node firmware |
| `/status` | `POST` | Accepts OTA progress/error JSON from the node and forwards it to the gateway |

---

## Serial Monitor

```bash
pio device monitor
```

Useful log messages include:

| Prefix / Example | Meaning |
|------------------|---------|
| `[C3] RX HELLO` | Gateway heartbeat / helper handshake |
| `[C3] RX UPLOAD_BEGIN` | Start of staged node firmware upload |
| `[C3] RX first UPLOAD_CHUNK` | First firmware chunk received from the gateway |
| `[C3] Staged ... / ... bytes` | Staging progress into LittleFS |
| `[C3] OTA AP started` | Temporary helper AP is running |
| `[C3] HTTP GET /firmware.bin` | Target node started downloading the staged image |
| `[C3] HTTP stream complete` | Full staged image was served to the node |
| `[C3] OTA helper abort received` | Gateway ended or aborted the helper session |

---

## Notes for Contributors

- This firmware should stay tightly aligned with the gateway Node OTA implementation.
- If you change `coproc_ota_protocol.h`, update both copies immediately.
- If you change helper AP behavior, chunk transport, or status reporting, test the full **gateway -> coprocessor -> node** OTA flow on real hardware.
- This helper is the foundation for future gateway-side offloaded features, so keep the design modular and well-scoped.
