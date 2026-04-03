/**
    * @file [main.cpp]
    * @brief Main source file for the (ESP32-C3) Gateway Coprocessor firmware
    * @version 0.3.0
    * @author Mrinal (@atechofficials)
 */

#define FW_VERSION "0.3.0"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <Update.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include "../../include/coproc_ota_protocol.h"
#include "../../include/mesh_protocol.h"
#include "../../include/user_config.h"

#define UART_BAUD 230400
#define UART_RX_BUFFER_SIZE 4096
#define UART_TX_BUFFER_SIZE 4096
#define OTA_FILE_PATH "/firmware.bin"
#define COPROC_OTA_HWCFG_MARKER "C3HWCFG:"
#define COPROC_OTA_FW_MARKER "C3FWVER:"
#define OTA_AP_IP IPAddress(192, 168, 4, 1)
#define OTA_AP_GW IPAddress(192, 168, 4, 1)
#define OTA_AP_MASK IPAddress(255, 255, 255, 0)
#define FRAME_MAX_PAYLOAD sizeof(CoprocUploadChunkPayload)
#define USB_SERIAL_WAIT_MS 250
#define SELF_OTA_REBOOT_DELAY_MS 400UL

__attribute__((used)) static const char kCoprocFirmwareVersionMarker[] =
    COPROC_OTA_FW_MARKER FW_VERSION;
__attribute__((used)) static const char kCoprocHardwareConfigMarker[] =
    COPROC_OTA_HWCFG_MARKER COPROC_HW_CONFIG_ID;
static volatile uint32_t gCoprocMarkerChecksum = 0;

static HardwareSerial linkUart(1);
static WebServer server(80);

struct HelperState {
    bool     fileReady = false;
    bool     apRunning = false;
    uint32_t sessionId = 0;
    uint32_t imageSize = 0;
    uint32_t imageCrc32 = 0;
    uint32_t runningCrc32 = 0xFFFFFFFFu;
    size_t   written = 0;
    uint8_t  nodeId = 0;
    uint8_t  nodeType = 0;
    uint8_t  apChannel = 6;
    uint16_t port = 80;
    char     version[COPROC_FW_VERSION_LEN] = {0};
    char     ssid[COPROC_SSID_LEN] = {0};
    char     password[COPROC_PASS_LEN] = {0};
    File     file;
} helper;

struct SelfOtaState {
    bool          active = false;
    bool          updateBegun = false;
    bool          rebootPending = false;
    uint32_t      sessionId = 0;
    uint32_t      imageSize = 0;
    uint32_t      imageCrc32 = 0;
    uint32_t      runningCrc32 = 0xFFFFFFFFu;
    size_t        written = 0;
    unsigned long rebootAtMs = 0;
    char          version[COPROC_FW_VERSION_LEN] = {0};
} selfOta;

struct RxState {
    enum State : uint8_t {
        WAIT_MAGIC_1 = 0,
        WAIT_MAGIC_2,
        READ_TYPE,
        READ_LEN_1,
        READ_LEN_2,
        READ_PAYLOAD,
        READ_CRC_1,
        READ_CRC_2,
    } state = WAIT_MAGIC_1;
    uint8_t  type = 0;
    uint16_t length = 0;
    uint16_t expectedCrc = 0;
    uint16_t payloadIndex = 0;
    uint8_t  payload[FRAME_MAX_PAYLOAD] = {0};
} rxState;

static void touchFirmwareMarkers() {
    uint32_t sum = 0;
    for (size_t i = 0; kCoprocFirmwareVersionMarker[i] != '\0'; i++) {
        sum += (uint8_t)kCoprocFirmwareVersionMarker[i];
    }
    for (size_t i = 0; kCoprocHardwareConfigMarker[i] != '\0'; i++) {
        sum += (uint8_t)kCoprocHardwareConfigMarker[i];
    }
    gCoprocMarkerChecksum = sum;
}

static uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static uint16_t crc16Ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void resetRxState() {
    rxState = RxState{};
}

static size_t getCoprocOtaSlotSize() {
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    return part ? part->size : 0;
}

static bool coprocOtaSupported() {
    return getCoprocOtaSlotSize() > 0;
}

static void fillInfoPayload(CoprocInfoPayload* info) {
    if (!info) return;
    memset(info, 0, sizeof(*info));
    info->ota_supported = coprocOtaSupported() ? 1 : 0;
    info->ota_max_bytes = (uint32_t)getCoprocOtaSlotSize();
    strncpy(info->fw_version, FW_VERSION, sizeof(info->fw_version) - 1);
    strncpy(info->hw_config_id, COPROC_HW_CONFIG_ID, sizeof(info->hw_config_id) - 1);
    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc && desc->project_name[0] != '\0') {
        strncpy(info->project_name, desc->project_name, sizeof(info->project_name) - 1);
    }
}

static void sendFrame(uint8_t type, const void* payload, size_t len) {
    if (len > FRAME_MAX_PAYLOAD) return;

    CoprocFrameHeader hdr{COPROC_FRAME_MAGIC, type, (uint16_t)len};
    linkUart.write((const uint8_t*)&hdr, sizeof(hdr));
    if (len > 0 && payload) linkUart.write((const uint8_t*)payload, len);

    uint8_t crcData[3 + FRAME_MAX_PAYLOAD] = {0};
    crcData[0] = type;
    crcData[1] = (uint8_t)(len & 0xFF);
    crcData[2] = (uint8_t)((len >> 8) & 0xFF);
    if (len > 0 && payload) memcpy(crcData + 3, payload, len);
    const uint16_t crc = crc16Ccitt(crcData, len + 3);
    const uint8_t crcBytes[2] = {
        (uint8_t)(crc & 0xFF),
        (uint8_t)((crc >> 8) & 0xFF)
    };
    linkUart.write(crcBytes, sizeof(crcBytes));
    linkUart.flush();
}

static void sendAck(uint8_t originalType, bool ok, uint32_t value, const char* message) {
    CoprocAckPayload ack{};
    ack.original_type = originalType;
    ack.ok = ok ? 1 : 0;
    ack.value = value;
    if (message) strncpy(ack.message, message, sizeof(ack.message) - 1);
    sendFrame(COPROC_FRAME_ACK, &ack, sizeof(ack));
}

static void sendStatus(uint32_t sessionId,
                       uint8_t nodeId,
                       uint8_t phase,
                       uint8_t progress,
                       uint8_t errorCode,
                       const char* message) {
    CoprocStatusPayload st{};
    st.session_id = sessionId;
    st.node_id = nodeId;
    st.phase = phase;
    st.progress = progress;
    st.error_code = errorCode;
    if (message) strncpy(st.message, message, sizeof(st.message) - 1);
    sendFrame(COPROC_FRAME_STATUS, &st, sizeof(st));
}

static void sendInfo() {
    CoprocInfoPayload info{};
    fillInfoPayload(&info);
    sendFrame(COPROC_FRAME_INFO, &info, sizeof(info));
}

static void logFrameRx(uint8_t type, size_t len) {
    switch (type) {
        case COPROC_FRAME_HELLO:
            Serial.printf("[C3] RX HELLO (%u B)\n", (unsigned)len);
            break;
        case COPROC_FRAME_UPLOAD_BEGIN:
            Serial.printf("[C3] RX UPLOAD_BEGIN (%u B)\n", (unsigned)len);
            break;
        case COPROC_FRAME_UPLOAD_CHUNK:
            if (helper.written == 0 && selfOta.written == 0) {
                Serial.printf("[C3] RX first UPLOAD_CHUNK (%u B)\n", (unsigned)len);
            }
            break;
        case COPROC_FRAME_UPLOAD_END:
            Serial.printf("[C3] RX UPLOAD_END (%u B)\n", (unsigned)len);
            break;
        case COPROC_FRAME_ABORT:
            Serial.printf("[C3] RX ABORT (%u B)\n", (unsigned)len);
            break;
        default:
            Serial.printf("[C3] RX frame type=0x%02X (%u B)\n", type, (unsigned)len);
            break;
    }
}

static void stopAccessPoint() {
    if (helper.apRunning) {
        server.stop();
        WiFi.softAPdisconnect(true);
        helper.apRunning = false;
    }
}

static void resetHelperState(bool removeFile) {
    if (helper.file) helper.file.close();
    stopAccessPoint();
    if (removeFile && LittleFS.exists(OTA_FILE_PATH)) LittleFS.remove(OTA_FILE_PATH);
    helper = HelperState{};
}

static void resetSelfOtaState(bool abortUpdate) {
    if (abortUpdate && selfOta.updateBegun) {
        Update.abort();
    }
    selfOta = SelfOtaState{};
}

static bool startAccessPoint() {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(OTA_AP_IP, OTA_AP_GW, OTA_AP_MASK);
    const bool ok = WiFi.softAP(helper.ssid, helper.password, helper.apChannel, false, 1);
    if (!ok) return false;

    Serial.printf("[C3] OTA AP started: SSID=%s CH=%u IP=%s\n",
                  helper.ssid,
                  helper.apChannel,
                  WiFi.softAPIP().toString().c_str());

    server.stop();
    server.on("/firmware.bin", HTTP_GET, []() {
        Serial.printf("[C3] HTTP GET /firmware.bin from %s\n", server.client().remoteIP().toString().c_str());
        if (!helper.fileReady || !LittleFS.exists(OTA_FILE_PATH)) {
            Serial.println("[C3] ERROR: firmware.bin requested before staging was ready.");
            server.send(404, "text/plain", "Firmware not staged");
            return;
        }
        File file = LittleFS.open(OTA_FILE_PATH, "r");
        if (!file) {
            Serial.println("[C3] ERROR: could not open staged firmware for HTTP GET.");
            server.send(500, "text/plain", "Open failed");
            return;
        }
        const size_t fileSize = file.size();
        Serial.printf("[C3] Streaming %u bytes of node firmware\n", (unsigned)fileSize);
        const size_t sent = server.streamFile(file, "application/octet-stream");
        Serial.printf("[C3] HTTP stream complete: sent=%u bytes\n", (unsigned)sent);
        file.close();
        if (sent == fileSize) {
            sendStatus(helper.sessionId,
                       helper.nodeId,
                       COPROC_HELPER_DONE,
                       90,
                       0,
                       "Firmware sent to node");
        }
    });

    server.on("/status", HTTP_POST, []() {
        if (!server.hasArg("plain")) {
            server.send(400, "application/json", R"({"ok":false,"error":"missing body"})");
            return;
        }

        JsonDocument doc;
        const auto err = deserializeJson(doc, server.arg("plain"));
        if (err != DeserializationError::Ok) {
            server.send(400, "application/json", R"({"ok":false,"error":"invalid json"})");
            return;
        }

        const uint32_t sessionId = doc["session_id"] | 0u;
        const uint8_t nodeId = doc["node_id"] | 0;
        const uint8_t phase = doc["phase"] | 0;
        const uint8_t progress = doc["progress"] | 0;
        const uint8_t errorCode = doc["error_code"] | 0;
        const char* message = doc["message"] | "";
        if (nodeId != 0) helper.nodeId = nodeId;

        Serial.printf("[C3] HTTP POST /status node=%u phase=%u progress=%u err=%u msg=\"%s\"\n",
                      nodeId, phase, progress, errorCode, message);
        sendStatus(sessionId, nodeId, phase, progress, errorCode, message);
        server.send(200, "application/json", R"({"ok":true})");
    });

    server.begin();
    helper.apRunning = true;
    return true;
}

static void reportSelfOtaError(uint32_t sessionId, uint8_t code, const char* message) {
    sendStatus(sessionId, 0, COPROC_HELPER_ERROR, 100, code, message);
}

static bool beginSelfUpdate(const CoprocUploadBeginPayload* begin) {
    resetHelperState(true);
    resetSelfOtaState(true);
    stopAccessPoint();
    WiFi.mode(WIFI_OFF);

    const size_t slotSize = getCoprocOtaSlotSize();
    if (slotSize == 0) {
        sendAck(COPROC_FRAME_UPLOAD_BEGIN, false, 0, "ota-unsupported");
        reportSelfOtaError(begin->session_id, 1, "Coprocessor OTA unsupported");
        return false;
    }
    if (begin->image_size == 0 || begin->image_size > slotSize) {
        sendAck(COPROC_FRAME_UPLOAD_BEGIN, false, (uint32_t)slotSize, "image-too-large");
        reportSelfOtaError(begin->session_id, 2, "Firmware image too large");
        return false;
    }
    if (!Update.begin(slotSize, U_FLASH)) {
        sendAck(COPROC_FRAME_UPLOAD_BEGIN, false, 0, "update-begin-failed");
        reportSelfOtaError(begin->session_id, 3, Update.errorString());
        return false;
    }

    selfOta.active = true;
    selfOta.updateBegun = true;
    selfOta.sessionId = begin->session_id;
    selfOta.imageSize = begin->image_size;
    selfOta.imageCrc32 = begin->image_crc32;
    selfOta.runningCrc32 = 0xFFFFFFFFu;
    selfOta.written = 0;
    strncpy(selfOta.version, begin->version, sizeof(selfOta.version) - 1);

    Serial.printf("[C3] Self OTA ready: %lu bytes, version=%s\n",
                  (unsigned long)selfOta.imageSize,
                  selfOta.version[0] ? selfOta.version : "(unknown)");
    sendAck(COPROC_FRAME_UPLOAD_BEGIN, true, (uint32_t)slotSize, "ready-self");
    sendStatus(selfOta.sessionId, 0, COPROC_HELPER_RECEIVING, 5, 0, "Ready for coprocessor OTA");
    return true;
}

static void handleNodeHelperUploadBegin(const CoprocUploadBeginPayload* begin) {
    resetSelfOtaState(true);
    resetHelperState(true);
    helper.sessionId = begin->session_id;
    helper.imageSize = begin->image_size;
    helper.imageCrc32 = begin->image_crc32;
    helper.nodeType = begin->node_type;
    helper.apChannel = begin->ap_channel;
    helper.port = begin->port;
    strncpy(helper.version, begin->version, sizeof(helper.version) - 1);
    strncpy(helper.ssid, begin->ssid, sizeof(helper.ssid) - 1);
    strncpy(helper.password, begin->password, sizeof(helper.password) - 1);
    helper.runningCrc32 = 0xFFFFFFFFu;
    helper.file = LittleFS.open(OTA_FILE_PATH, "w");
    if (!helper.file) {
        Serial.println("[C3] ERROR: failed to open OTA staging file.");
        sendAck(COPROC_FRAME_UPLOAD_BEGIN, false, 0, "fs-open-failed");
        reportSelfOtaError(begin->session_id, 4, "Node OTA staging open failed");
        return;
    }
    Serial.printf("[C3] OTA staging ready: %lu bytes, nodeType=%u, version=%s\n",
                  (unsigned long)helper.imageSize,
                  helper.nodeType,
                  helper.version);
    sendAck(COPROC_FRAME_UPLOAD_BEGIN, true, 0, "ready");
}

static void handleSelfOtaChunk(const uint8_t* payload, size_t len) {
    if (!selfOta.active || !selfOta.updateBegun || len < sizeof(uint32_t)) {
        sendAck(COPROC_FRAME_UPLOAD_CHUNK, false, (uint32_t)selfOta.written, "bad-self-chunk");
        reportSelfOtaError(selfOta.sessionId, 5, "Bad self OTA chunk state");
        return;
    }

    const uint32_t offset = *(const uint32_t*)payload;
    const uint8_t* chunk = payload + sizeof(uint32_t);
    const size_t chunkLen = len - sizeof(uint32_t);
    if (offset != selfOta.written) {
        Serial.printf("[C3] ERROR: self OTA chunk offset mismatch got=%lu expected=%u\n",
                      (unsigned long)offset, (unsigned)selfOta.written);
        sendAck(COPROC_FRAME_UPLOAD_CHUNK, false, (uint32_t)selfOta.written, "offset-mismatch");
        reportSelfOtaError(selfOta.sessionId, 6, "Self OTA chunk offset mismatch");
        return;
    }

    const size_t written = Update.write(const_cast<uint8_t*>(chunk), chunkLen);
    if (written != chunkLen) {
        Serial.printf("[C3] ERROR: self OTA chunk write failed at offset=%u len=%u\n",
                      (unsigned)selfOta.written, (unsigned)chunkLen);
        sendAck(COPROC_FRAME_UPLOAD_CHUNK, false, (uint32_t)selfOta.written, "write-failed");
        reportSelfOtaError(selfOta.sessionId, 7, Update.errorString());
        return;
    }

    selfOta.written += chunkLen;
    selfOta.runningCrc32 = crc32Update(selfOta.runningCrc32, chunk, chunkLen);
    if ((selfOta.written % 65536u) == 0 || selfOta.written == selfOta.imageSize) {
        const uint8_t progress = (uint8_t)std::min<size_t>(85, 5 + ((selfOta.written * 80u) / std::max<size_t>(1, selfOta.imageSize)));
        Serial.printf("[C3] Self OTA received %u / %u bytes\n",
                      (unsigned)selfOta.written, (unsigned)selfOta.imageSize);
        sendStatus(selfOta.sessionId, 0, COPROC_HELPER_RECEIVING, progress, 0, "Receiving coprocessor firmware");
    }
    sendAck(COPROC_FRAME_UPLOAD_CHUNK, true, (uint32_t)selfOta.written, "ok");
}

static void handleNodeHelperChunk(const uint8_t* payload, size_t len) {
    if (len < sizeof(uint32_t) || !helper.file) {
        Serial.println("[C3] ERROR: bad upload chunk state.");
        sendAck(COPROC_FRAME_UPLOAD_CHUNK, false, (uint32_t)helper.written, "bad-chunk");
        return;
    }
    const uint32_t offset = *(const uint32_t*)payload;
    const uint8_t* chunk = payload + sizeof(uint32_t);
    const size_t chunkLen = len - sizeof(uint32_t);
    if (offset != helper.written) {
        Serial.printf("[C3] ERROR: chunk offset mismatch got=%lu expected=%u\n",
                      (unsigned long)offset, (unsigned)helper.written);
        sendAck(COPROC_FRAME_UPLOAD_CHUNK, false, (uint32_t)helper.written, "offset-mismatch");
        return;
    }
    if (helper.file.write(chunk, chunkLen) != chunkLen) {
        Serial.printf("[C3] ERROR: chunk write failed at offset=%u len=%u\n",
                      (unsigned)helper.written, (unsigned)chunkLen);
        sendAck(COPROC_FRAME_UPLOAD_CHUNK, false, (uint32_t)helper.written, "write-failed");
        return;
    }
    helper.written += chunkLen;
    helper.runningCrc32 = crc32Update(helper.runningCrc32, chunk, chunkLen);
    if ((helper.written % 65536u) == 0 || helper.written == helper.imageSize) {
        Serial.printf("[C3] Staged %u / %u bytes\n", (unsigned)helper.written, (unsigned)helper.imageSize);
    }
    sendAck(COPROC_FRAME_UPLOAD_CHUNK, true, (uint32_t)helper.written, "ok");
}

static void finalizeSelfUpdate() {
    if (!selfOta.active || !selfOta.updateBegun) {
        sendAck(COPROC_FRAME_UPLOAD_END, false, 0, "bad-self-end");
        reportSelfOtaError(selfOta.sessionId, 8, "Self OTA finalize without active upload");
        return;
    }
    if (selfOta.written != selfOta.imageSize) {
        Serial.printf("[C3] ERROR: self OTA size mismatch wrote=%u expected=%u\n",
                      (unsigned)selfOta.written, (unsigned)selfOta.imageSize);
        sendAck(COPROC_FRAME_UPLOAD_END, false, (uint32_t)selfOta.written, "size-mismatch");
        reportSelfOtaError(selfOta.sessionId, 9, "Self OTA size mismatch");
        resetSelfOtaState(true);
        return;
    }
    if (selfOta.runningCrc32 != selfOta.imageCrc32) {
        Serial.printf("[C3] ERROR: self OTA crc mismatch got=0x%08lX expected=0x%08lX\n",
                      (unsigned long)selfOta.runningCrc32,
                      (unsigned long)selfOta.imageCrc32);
        sendAck(COPROC_FRAME_UPLOAD_END, false, selfOta.runningCrc32, "crc-mismatch");
        reportSelfOtaError(selfOta.sessionId, 10, "Self OTA CRC mismatch");
        resetSelfOtaState(true);
        return;
    }

    sendStatus(selfOta.sessionId, 0, COPROC_HELPER_FLASHING, 90, 0, "Flashing coprocessor firmware...");
    if (!Update.end(true)) {
        const char* err = Update.errorString();
        Serial.printf("[C3] ERROR: self OTA finalization failed: %s\n", err);
        sendAck(COPROC_FRAME_UPLOAD_END, false, 0, "finalize-failed");
        reportSelfOtaError(selfOta.sessionId, 11, err);
        resetSelfOtaState(true);
        return;
    }
    if (!Update.isFinished()) {
        Serial.println("[C3] ERROR: self OTA did not finish.");
        sendAck(COPROC_FRAME_UPLOAD_END, false, 0, "not-finished");
        reportSelfOtaError(selfOta.sessionId, 12, "Self OTA did not finish");
        resetSelfOtaState(true);
        return;
    }

    selfOta.updateBegun = false;
    selfOta.rebootPending = true;
    selfOta.rebootAtMs = millis() + SELF_OTA_REBOOT_DELAY_MS;
    Serial.printf("[C3] Self OTA complete. Rebooting into version %s\n",
                  selfOta.version[0] ? selfOta.version : "(unknown)");
    sendAck(COPROC_FRAME_UPLOAD_END, true, 0, "update-complete");
    sendStatus(selfOta.sessionId, 0, COPROC_HELPER_DONE, 95, 0, "Firmware flashed. Rebooting coprocessor...");
}

static void finalizeNodeHelperUpload() {
    if (helper.file) helper.file.close();
    if (helper.written != helper.imageSize) {
        Serial.printf("[C3] ERROR: size mismatch wrote=%u expected=%u\n",
                      (unsigned)helper.written, (unsigned)helper.imageSize);
        sendAck(COPROC_FRAME_UPLOAD_END, false, (uint32_t)helper.written, "size-mismatch");
        return;
    }
    if (helper.runningCrc32 != helper.imageCrc32) {
        Serial.printf("[C3] ERROR: crc mismatch got=0x%08lX expected=0x%08lX\n",
                      (unsigned long)helper.runningCrc32,
                      (unsigned long)helper.imageCrc32);
        sendAck(COPROC_FRAME_UPLOAD_END, false, helper.runningCrc32, "crc-mismatch");
        return;
    }
    helper.fileReady = true;
    if (!startAccessPoint()) {
        Serial.println("[C3] ERROR: failed to start OTA AP.");
        sendAck(COPROC_FRAME_UPLOAD_END, false, 0, "ap-start-failed");
        return;
    }
    Serial.println("[C3] OTA helper ready for node download.");
    sendAck(COPROC_FRAME_UPLOAD_END, true, 0, "ap-ready");
    sendStatus(helper.sessionId, 0, COPROC_HELPER_AP_READY, 100, 0, "OTA AP ready");
}

static void handleFrame(uint8_t type, const uint8_t* payload, size_t len) {
    logFrameRx(type, len);

    switch (type) {
        case COPROC_FRAME_HELLO:
            sendAck(COPROC_FRAME_HELLO, true, helper.apRunning ? 1u : (selfOta.active ? 2u : 0u),
                    selfOta.active ? "self-ota" : "helper-ready");
            sendInfo();
            if (helper.apRunning) {
                sendStatus(helper.sessionId, 0, COPROC_HELPER_AP_READY, 100, 0, "OTA AP ready");
            }
            break;

        case COPROC_FRAME_UPLOAD_BEGIN: {
            if (len < sizeof(CoprocUploadBeginPayload)) {
                sendAck(COPROC_FRAME_UPLOAD_BEGIN, false, 0, "bad-begin");
                return;
            }
            const auto* begin = reinterpret_cast<const CoprocUploadBeginPayload*>(payload);
            if (begin->upload_target == COPROC_UPLOAD_TARGET_SELF) {
                beginSelfUpdate(begin);
            } else if (begin->upload_target == COPROC_UPLOAD_TARGET_NODE_HELPER) {
                handleNodeHelperUploadBegin(begin);
            } else {
                sendAck(COPROC_FRAME_UPLOAD_BEGIN, false, 0, "bad-target");
            }
            break;
        }

        case COPROC_FRAME_UPLOAD_CHUNK:
            if (selfOta.active) handleSelfOtaChunk(payload, len);
            else handleNodeHelperChunk(payload, len);
            break;

        case COPROC_FRAME_UPLOAD_END:
            if (selfOta.active) finalizeSelfUpdate();
            else finalizeNodeHelperUpload();
            break;

        case COPROC_FRAME_ABORT:
            Serial.println("[C3] OTA helper abort received.");
            resetHelperState(true);
            resetSelfOtaState(true);
            WiFi.mode(WIFI_OFF);
            sendAck(COPROC_FRAME_ABORT, true, 0, "aborted");
            break;
    }
}

static void processSerial() {
    while (linkUart.available() > 0) {
        const uint8_t b = (uint8_t)linkUart.read();
        switch (rxState.state) {
            case RxState::WAIT_MAGIC_1:
                if (b == (COPROC_FRAME_MAGIC & 0xFF)) rxState.state = RxState::WAIT_MAGIC_2;
                break;
            case RxState::WAIT_MAGIC_2:
                if (b == ((COPROC_FRAME_MAGIC >> 8) & 0xFF)) rxState.state = RxState::READ_TYPE;
                else rxState.state = RxState::WAIT_MAGIC_1;
                break;
            case RxState::READ_TYPE:
                rxState.type = b;
                rxState.state = RxState::READ_LEN_1;
                break;
            case RxState::READ_LEN_1:
                rxState.length = b;
                rxState.state = RxState::READ_LEN_2;
                break;
            case RxState::READ_LEN_2:
                rxState.length |= (uint16_t)b << 8;
                if (rxState.length > FRAME_MAX_PAYLOAD) {
                    Serial.printf("[C3] ERROR: oversize frame len=%u max=%u\n",
                                  (unsigned)rxState.length,
                                  (unsigned)FRAME_MAX_PAYLOAD);
                    resetRxState();
                }
                else if (rxState.length == 0) rxState.state = RxState::READ_CRC_1;
                else {
                    rxState.payloadIndex = 0;
                    rxState.state = RxState::READ_PAYLOAD;
                }
                break;
            case RxState::READ_PAYLOAD:
                rxState.payload[rxState.payloadIndex++] = b;
                if (rxState.payloadIndex >= rxState.length) rxState.state = RxState::READ_CRC_1;
                break;
            case RxState::READ_CRC_1:
                rxState.expectedCrc = b;
                rxState.state = RxState::READ_CRC_2;
                break;
            case RxState::READ_CRC_2: {
                rxState.expectedCrc |= (uint16_t)b << 8;
                uint8_t crcData[3 + FRAME_MAX_PAYLOAD] = {0};
                crcData[0] = rxState.type;
                crcData[1] = (uint8_t)(rxState.length & 0xFF);
                crcData[2] = (uint8_t)((rxState.length >> 8) & 0xFF);
                if (rxState.length > 0) memcpy(crcData + 3, rxState.payload, rxState.length);
                const uint16_t actual = crc16Ccitt(crcData, rxState.length + 3);
                if (actual == rxState.expectedCrc) {
                    handleFrame(rxState.type, rxState.payload, rxState.length);
                } else {
                    Serial.printf("[C3] ERROR: CRC mismatch type=0x%02X len=%u got=0x%04X expected=0x%04X\n",
                                  (unsigned)rxState.type,
                                  (unsigned)rxState.length,
                                  (unsigned)actual,
                                  (unsigned)rxState.expectedCrc);
                }
                resetRxState();
                break;
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    const unsigned long waitStart = millis();
    while (!Serial && (millis() - waitStart) < USB_SERIAL_WAIT_MS) {
        delay(10);
    }
    Serial.printf("\n[C3] OTA helper booting v%s\n", FW_VERSION);
    touchFirmwareMarkers();

    pinMode(REBOOT_SIGNAL_PIN, INPUT_PULLDOWN);
    if (!LittleFS.begin(true)) {
        Serial.println("[C3] ERROR: LittleFS mount failed.");
    } else {
        Serial.println("[C3] LittleFS mounted.");
    }

    WiFi.mode(WIFI_OFF);
    linkUart.setRxBufferSize(UART_RX_BUFFER_SIZE);
    linkUart.setTxBufferSize(UART_TX_BUFFER_SIZE);
    linkUart.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.printf("[C3] UART link ready: baud=%u RX=%u TX=%u reboot-pin=%u rxbuf=%u txbuf=%u\n",
                  (unsigned)UART_BAUD,
                  (unsigned)UART_RX_PIN,
                  (unsigned)UART_TX_PIN,
                  (unsigned)REBOOT_SIGNAL_PIN,
                  (unsigned)UART_RX_BUFFER_SIZE,
                  (unsigned)UART_TX_BUFFER_SIZE);
    resetRxState();
}

void loop() {
    static bool lastResetSignal = false;

    processSerial();
    if (helper.apRunning) server.handleClient();

    const bool resetSignal = digitalRead(REBOOT_SIGNAL_PIN) == HIGH;
    if (resetSignal && !lastResetSignal) {
        Serial.println("[C3] Reboot signal received from gateway.");
        delay(20);
        ESP.restart();
    }
    lastResetSignal = resetSignal;

    if (selfOta.rebootPending && millis() >= selfOta.rebootAtMs) {
        Serial.println("[C3] Restarting into updated coprocessor firmware.");
        delay(20);
        ESP.restart();
    }

    delay(2);
}




