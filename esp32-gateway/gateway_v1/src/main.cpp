/**
    * @file [main.cpp]
    * @brief Main source file for the ESP32 Mesh Gateway firmware
    * @version 2.1.4
    * @author Mrinal (@atechofficials)
 */
#define FW_VERSION "2.1.4"
#define HW_CONFIG_ID "0x0A"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <Update.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include "mesh_protocol.h"
#include "coproc_ota_protocol.h"
#include <Preferences.h>
#include "mbedtls/sha256.h"
#include <algorithm>
#include <ctype.h>
#include <set>

// Configuration
#define RESET_BTN_PIN    0
#define AP_SSID_DEFAULT  "ESP32-Mesh-Gateway"
#define AP_PASS_DEFAULT  "meshsetup"
#define WEB_PORT         80
#define RELAY_LABEL_MAX_LEN 25
#define OTA_DESC_BUF_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
#define OTA_FW_MARKER "GWFWVER:"
#define OTA_FW_MARKER_LEN 8
#define OTA_FW_VERSION_MAX_LEN 16
#define OTA_HWCFG_MARKER "GWHWCFG:"
#define OTA_HWCFG_MARKER_LEN 8
#define OTA_HWCFG_ID_MAX_LEN HW_CONFIG_ID_LEN
#define NODE_OTA_FW_MARKER "NODEFWVER:"
#define NODE_OTA_FW_MARKER_LEN 10
#define NODE_OTA_ROLE_MARKER "NODETYPE:"
#define NODE_OTA_ROLE_MARKER_LEN 9
#define NODE_OTA_HWCFG_MARKER "NODEHWCFG:"
#define NODE_OTA_HWCFG_MARKER_LEN 10
#define NODE_OTA_VERSION_MAX_LEN 16
#define NODE_OTA_FILE_PATH "/node_ota.bin"
#define NODE_OTA_HOST "192.168.4.1"
#define NODE_OTA_PORT 80
#define NODE_OTA_AP_CHANNEL 6
#define NODE_OTA_STAGE_TIMEOUT_MS 120000UL
#define NODE_OTA_TRANSFER_TIMEOUT_MS 300000UL
#define NODE_OTA_REJOIN_TIMEOUT_MS 90000UL
#define NODE_OTA_TIMEOUT_RECOVERY_MS 120000UL
#define NODE_OTA_BEGIN_RETRY_MS 2000UL
#define NODE_OTA_BEGIN_MAX_ATTEMPTS 10
#define COPROC_UART_BAUD 230400
#define COPROC_UART_TX_PIN 4
#define COPROC_UART_RX_PIN 5
#define COPROC_UART_RX_BUFFER_SIZE 4096
#define COPROC_UART_TX_BUFFER_SIZE 4096
#define COPROC_RESET_PIN 1
#define COPROC_LINK_PING_MS 5000UL
#define COPROC_ACK_TIMEOUT_MS 3000UL
#define COPROC_RECOVERY_REBOOT_MS 8000UL
#define COPROC_RECOVERY_COOLDOWN_MS 15000UL
#define COPROC_ACK_RETRY_LIMIT 3
#define COPROC_FRAME_MAX_PAYLOAD sizeof(CoprocUploadChunkPayload)

// Runtime AP credentials - loaded from NVS on boot, fall back to compile-time defaults.
// These are the SSID / password the WiFiManager captive portal AP will use.
static char gwApSsid[33]     = AP_SSID_DEFAULT;
static char gwApPassword[64] = AP_PASS_DEFAULT;

// Web Interface Auth
static char webUsername[33]   = "";   // max 32 chars
static char webPassHash[65]   = "";   // SHA-256 hex of password
static char sessionToken[65]  = "";   // generated at login, lives in RAM only
static char rememberToken[65] = "";   // NVS-persisted, used for "Remember Me"
static std::set<uint32_t> authWsClients;  // authenticated WS client IDs

#define RX_QUEUE_SIZE    30
#define WS_UPDATE_MS     2000
#define WS_META_MS       10000
#define GW_LED_PIN       38
#define MAX_DISCOVERED   10
#define RFID_UID_HEX_MAX_LEN ((RFID_UID_MAX_LEN * 2) + 1)

// Node Registry
struct GatewayRfidSlot {
    bool     enabled = false;
    uint8_t  uidLen = 0;
    uint16_t relayMask = 0;
    uint8_t  uid[RFID_UID_MAX_LEN] = {0};
};

struct NodeRecord {
    uint8_t       mac[6];
    NodeType      type;
    uint32_t      capabilities;
    char          name[MESH_NODE_NAME_LEN];
    char          fw_version[8];  // reported by node in MSG_REGISTER
    char          hw_config_id[HW_CONFIG_ID_LEN];  // reported by node in MSG_REGISTER
    unsigned long lastSeen;
    bool          online;
    uint32_t      uptime;
    uint8_t actuatorMask;
    uint8_t       actuatorCount;
    // Per-node dynamic settings schema (populated by MSG_SETTINGS_DATA)
    uint8_t       settingsCount;                     // 0 = not yet received
    SettingDef    settings[NODE_MAX_SETTINGS];        // schema + current values
    // Per-node dynamic sensor schema (populated by MSG_SENSOR_SCHEMA)
    // Indexed by position; sensorSchema[j] and sensorValues[j] are always parallel.
    // sensorCount == 0 means schema has not yet been received from this node.
    uint8_t       sensorCount;                       // 0 = schema not yet received
    SensorDef     sensorSchema[NODE_MAX_SENSORS];    // descriptor for each sensor channel
    float         sensorValues[NODE_MAX_SENSORS];    // last received value per channel
    ActuatorDef   actuatorSchema[NODE_MAX_ACTUATORS];
    char          relayLabels[NODE_MAX_ACTUATORS][RELAY_LABEL_MAX_LEN];
    bool          rfidConfigReady;
    GatewayRfidSlot rfidSlots[RFID_MAX_SLOTS];
    uint8_t       lastRfidUid[RFID_UID_MAX_LEN];
    uint8_t       lastRfidUidLen;
    int8_t        lastRfidMatchedSlot;
    uint16_t      lastRfidAppliedMask;
    unsigned long lastRfidSeenAt;
};
static NodeRecord nodes[MESH_MAX_NODES + 1];
static uint8_t    nextId = 1;
static bool       nodeRegistryDirty = false;
static unsigned long nodeRegistryDirtyAtMs = 0;
static constexpr unsigned long NODE_REGISTRY_FLUSH_MS = 750UL;

// Discovered (beaconing) nodes - not yet paired
struct DiscoveredNode {
    uint8_t       mac[6];
    char          name[MESH_NODE_NAME_LEN];
    NodeType      type;
    uint8_t       tx_channel;  // channel from last beacon
    unsigned long lastSeen;
    bool          active;
};
static DiscoveredNode discovered[MAX_DISCOVERED];

// Pending pair - gateway retries PAIR_CMD until MSG_REGISTER arrives
struct PendingPair {
    uint8_t       mac[6];
    bool          active;
    unsigned long startedAt;
    unsigned long lastAttempt;
};
static PendingPair pendingPair = {};

// RTOS
static SemaphoreHandle_t nodesMutex;

struct RxPacket { uint8_t mac[6]; uint8_t data[250]; int len; };
static QueueHandle_t rxQueue;

// Web Server
static AsyncWebServer server(WEB_PORT);
static AsyncWebSocket ws("/ws");

// State
static uint8_t   wifiChannel = 1;
static uint32_t  bootMs      = 0;

// Gateway Status LED
static Adafruit_NeoPixel gwLed(1, GW_LED_PIN, NEO_GRB + NEO_KHZ800);
static uint32_t          gwLedCurrent   = 0xDEADBEEF;
static unsigned long     gwLedFlashUntil = 0;
bool gwLedEnabled = true;
__attribute__((used)) static const char kGatewayFirmwareVersionMarker[] = OTA_FW_MARKER FW_VERSION;
__attribute__((used)) static const char kGatewayHardwareConfigMarker[] = OTA_HWCFG_MARKER HW_CONFIG_ID;
static volatile uint32_t gGatewayFirmwareMarkerChecksum = 0;

static void touchGatewayFirmwareMarkers() {
    uint32_t sum = 0;
    for (size_t i = 0; kGatewayFirmwareVersionMarker[i] != '\0'; i++) {
        sum += (uint8_t)kGatewayFirmwareVersionMarker[i];
    }
    for (size_t i = 0; kGatewayHardwareConfigMarker[i] != '\0'; i++) {
        sum += (uint8_t)kGatewayHardwareConfigMarker[i];
    }
    gGatewayFirmwareMarkerChecksum = sum;
}

struct GatewayOtaRequestState {
    bool   started = false;
    bool   ok = false;
    bool   updateBegun = false;
    bool   metadataValidated = false;
    bool   descFlushed = false;
    size_t expectedSize = 0;
    size_t written = 0;
    char   filename[64] = {0};
    char   incomingVersion[33] = {0};
    char   incomingDisplayVersion[OTA_FW_VERSION_MAX_LEN] = {0};
    char   incomingHwConfigId[OTA_HWCFG_ID_MAX_LEN] = {0};
    char   error[128] = {0};
    uint8_t descBuf[OTA_DESC_BUF_LEN] = {0};
    size_t descBytes = 0;
    uint8_t markerMatchIndex = 0;
    bool    markerReadingVersion = false;
    uint8_t markerVersionLen = 0;
    uint8_t hwMarkerMatchIndex = 0;
    bool    hwMarkerReading = false;
    uint8_t hwMarkerLen = 0;
};

static bool          otaUploadBusy    = false;
static bool          otaRebootPending = false;
static unsigned long otaRebootAtMs    = 0;

enum NodeOtaJobStage : uint8_t {
    NODE_OTA_JOB_IDLE = 0,
    NODE_OTA_JOB_STAGED,
    NODE_OTA_JOB_COPROC_WAIT,
    NODE_OTA_JOB_COPROC_BEGIN,
    NODE_OTA_JOB_COPROC_STREAM,
    NODE_OTA_JOB_COPROC_FINALIZE,
    NODE_OTA_JOB_SEND_COMMAND,
    NODE_OTA_JOB_WAIT_TRANSFER,
    NODE_OTA_JOB_WAIT_REJOIN,
    NODE_OTA_JOB_SUCCESS,
    NODE_OTA_JOB_ERROR,
};

struct NodeOtaUploadRequestState {
    bool   started = false;
    bool   ok = false;
    bool   metadataValidated = false;
    bool   descFlushed = false;
    size_t expectedSize = 0;
    size_t written = 0;
    uint32_t crc32 = 0xFFFFFFFFu;
    char   filename[64] = {0};
    char   incomingVersion[NODE_OTA_VERSION_MAX_LEN] = {0};
    char   incomingDisplayVersion[NODE_OTA_VERSION_MAX_LEN] = {0};
    char   incomingRole[16] = {0};
    char   incomingHwConfigId[HW_CONFIG_ID_LEN] = {0};
    char   error[128] = {0};
    uint8_t descBuf[OTA_DESC_BUF_LEN] = {0};
    size_t descBytes = 0;
    uint8_t versionMarkerMatchIndex = 0;
    bool    versionMarkerReading = false;
    uint8_t versionMarkerLen = 0;
    uint8_t roleMarkerMatchIndex = 0;
    bool    roleMarkerReading = false;
    uint8_t roleMarkerLen = 0;
    uint8_t hwMarkerMatchIndex = 0;
    bool    hwMarkerReading = false;
    uint8_t hwMarkerLen = 0;
    File    file;
};

struct NodeOtaJobState {
    bool           active = false;
    bool           uploadBusy = false;
    bool           helperOnline = false;
    bool           awaitingAck = false;
    bool           helperApReady = false;
    bool           helperTransferDone = false;
    bool           nodeAccepted = false;
    uint8_t        awaitingFrameType = 0;
    uint8_t        nodeId = 0;
    NodeType       targetType = NODE_SENSOR;
    NodeOtaJobStage stage = NODE_OTA_JOB_IDLE;
    uint32_t       sessionId = 0;
    uint32_t       imageCrc32 = 0;
    size_t         imageSize = 0;
    size_t         uploadOffset = 0;
    unsigned long  lastCommandAt = 0;
    unsigned long  stageStartedAt = 0;
    unsigned long  lastActivityAt = 0;
    unsigned long  lastAckAt = 0;
    uint8_t        coprocAckRetries = 0;
    uint8_t        commandAttempts = 0;
    char           version[NODE_OTA_VERSION_MAX_LEN] = {0};
    char           priorVersion[8] = {0};
    char           filename[64] = {0};
    char           message[96] = {0};
    char           error[128] = {0};
    char           apSsid[NODE_OTA_SSID_LEN] = {0};
    char           apPassword[NODE_OTA_PASS_LEN] = {0};
    uint8_t        progress = 0;
    File           file;
};

struct CoprocAckState {
    bool     ready = false;
    uint8_t  originalType = 0;
    bool     ok = false;
    uint32_t value = 0;
    char     message[COPROC_STATUS_MESSAGE_LEN] = {0};
};

struct CoprocRxState {
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
    uint8_t  payload[COPROC_FRAME_MAX_PAYLOAD] = {0};
};

static HardwareSerial coprocSerial(1);
static NodeOtaJobState nodeOtaJob;
static CoprocAckState coprocAck;
static CoprocRxState coprocRx;
static unsigned long lastCoprocHelloAt = 0;
static bool coprocOnline = false;
static unsigned long lastCoprocAckAt = 0;
static unsigned long lastCoprocNoAckLogAt = 0;
static unsigned long lastCoprocRecoveryPulseAt = 0;

// *****************************************************************************
// Gateway Status LED Helpers
// *****************************************************************************
static void setGwLed(uint32_t color) {
    if (color == gwLedCurrent) return;
    gwLedCurrent = color;
    gwLed.setPixelColor(0, color);
    gwLed.show();
}

static void flashGwLed(uint32_t color, uint32_t durationMs) {
    gwLedFlashUntil = millis() + durationMs;
    setGwLed(color);
    gwLedCurrent = 0xDEADBEEF;  // force refresh after flash
}

static void updateGwLed() {
    if (!gwLedEnabled) return;
    if (millis() < gwLedFlashUntil) return;
    setGwLed(gwLed.Color(0, 0, 32));  // dim blue = operational
}

static void saveGwLedState(bool enabled) {
    Preferences prefs;
    prefs.begin("gwconfig", false);  // read-write
    prefs.putBool("led_enabled", enabled);
    prefs.end();
    gwLedEnabled = enabled;
    if (!enabled) {
        setGwLed(0);  // turn off immediately
    }
}

static void loadGwLedState() {
    Preferences prefs;
    prefs.begin("gwconfig", true);  // read-only
    gwLedEnabled = prefs.getBool("led_enabled", true);
    prefs.end();
    Serial.printf("[CFG]  Gateway LED is %s\n", gwLedEnabled ? "enabled" : "disabled");
    if (!gwLedEnabled) {
        setGwLed(0);
    }
}

// *****************************************************************************
//  Helpers
// *****************************************************************************
static String macToStr(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

static bool parseMac(const char* str, uint8_t* mac) {
    return sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6;
}

static void defaultRelayLabel(uint8_t relayIdx, char* out, size_t outSize) {
    snprintf(out, outSize, "Relay %u", relayIdx + 1);
}

static void resetRelayLabels(char labels[NODE_MAX_ACTUATORS][RELAY_LABEL_MAX_LEN]) {
    for (uint8_t i = 0; i < NODE_MAX_ACTUATORS; i++) {
        defaultRelayLabel(i, labels[i], RELAY_LABEL_MAX_LEN);
    }
}

static void sanitizeRelayLabel(const char* input, uint8_t relayIdx,
                               char* out, size_t outSize) {
    String label = input ? String(input) : String();
    label.trim();
    if (label.length() == 0) {
        defaultRelayLabel(relayIdx, out, outSize);
        return;
    }
    label.replace("\r", " ");
    label.replace("\n", " ");
    while (label.indexOf("  ") >= 0) label.replace("  ", " ");
    strncpy(out, label.c_str(), outSize - 1);
    out[outSize - 1] = '\0';
}

static void buildRelayLabelPrefKey(const uint8_t* mac, char* key, size_t keySize) {
    snprintf(key, keySize, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

struct RelayLabelRecordV1 {
    char labels[4][RELAY_LABEL_MAX_LEN];
};

struct RelayLabelRecordV2 {
    char labels[NODE_MAX_ACTUATORS][RELAY_LABEL_MAX_LEN];
};

static void loadRelayLabelsForNode(uint8_t nodeId) {
    if (nodeId == 0 || nodeId > MESH_MAX_NODES) return;
    resetRelayLabels(nodes[nodeId].relayLabels);
    if (nodes[nodeId].mac[0] == 0 && nodes[nodeId].mac[1] == 0) return;

    RelayLabelRecordV2 rec{};
    char key[13];
    buildRelayLabelPrefKey(nodes[nodeId].mac, key, sizeof(key));

    Preferences prefs;
    prefs.begin("gwrelay", true);
    const size_t blobLen = prefs.getBytesLength(key);
    size_t got = 0;
    if (blobLen == sizeof(RelayLabelRecordV2)) {
        got = prefs.getBytes(key, &rec, sizeof(rec));
    } else if (blobLen == sizeof(RelayLabelRecordV1)) {
        RelayLabelRecordV1 legacy{};
        got = prefs.getBytes(key, &legacy, sizeof(legacy));
        if (got == sizeof(legacy)) {
            for (uint8_t i = 0; i < 4; i++) {
                memcpy(rec.labels[i], legacy.labels[i], sizeof(legacy.labels[i]));
            }
        }
    }
    prefs.end();

    if (got != sizeof(rec) && got != sizeof(RelayLabelRecordV1)) return;

    for (uint8_t i = 0; i < NODE_MAX_ACTUATORS; i++) {
        sanitizeRelayLabel(rec.labels[i], i, nodes[nodeId].relayLabels[i], RELAY_LABEL_MAX_LEN);
    }
}

static void saveRelayLabelsForNode(uint8_t nodeId) {
    if (nodeId == 0 || nodeId > MESH_MAX_NODES) return;
    if (nodes[nodeId].mac[0] == 0 && nodes[nodeId].mac[1] == 0) return;

    RelayLabelRecordV2 rec{};
    for (uint8_t i = 0; i < NODE_MAX_ACTUATORS; i++) {
        sanitizeRelayLabel(nodes[nodeId].relayLabels[i], i, rec.labels[i], RELAY_LABEL_MAX_LEN);
    }

    char key[13];
    buildRelayLabelPrefKey(nodes[nodeId].mac, key, sizeof(key));

    Preferences prefs;
    prefs.begin("gwrelay", false);
    prefs.putBytes(key, &rec, sizeof(rec));
    prefs.end();
}

static void removeRelayLabelsForMac(const uint8_t* mac) {
    char key[13];
    buildRelayLabelPrefKey(mac, key, sizeof(key));
    Preferences prefs;
    prefs.begin("gwrelay", false);
    prefs.remove(key);
    prefs.end();
}

static uint8_t findNodeByMac(const uint8_t* mac) {
    for (uint8_t i = 1; i < nextId; i++) {
        if (nodes[i].mac[0] == 0 && nodes[i].mac[1] == 0) continue; // empty slot
        if (memcmp(nodes[i].mac, mac, 6) == 0) return i;
    }
    return 0;
}

// Find an empty/recycled slot or allocate a new one
static uint8_t findFreeSlot() {
    for (uint8_t i = 1; i < nextId; i++) {
        if (nodes[i].mac[0] == 0 && nodes[i].mac[1] == 0) return i;
    }
    if (nextId > MESH_MAX_NODES) return 0;
    return nextId++;
}

static uint8_t peekFreeSlot() {
    uint8_t slot = 0;
    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint8_t i = 1; i < nextId; i++) {
            if (nodes[i].mac[0] == 0 && nodes[i].mac[1] == 0) {
                slot = i;
                break;
            }
        }
        if (slot == 0 && nextId <= MESH_MAX_NODES) slot = nextId;
        xSemaphoreGive(nodesMutex);
    }
    return slot;
}

static uint8_t countRegisteredNodes() {
    uint8_t count = 0;
    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint8_t i = 1; i < nextId; i++) {
            if (nodes[i].mac[0] == 0 && nodes[i].mac[1] == 0) continue;
            count++;
        }
        xSemaphoreGive(nodesMutex);
    }
    return count;
}

static String buildPairCapacityFullJson(const char* macStr = nullptr) {
    JsonDocument doc;
    doc["type"] = "pair_capacity_full";
    doc["current_nodes"] = countRegisteredNodes();
    doc["max_nodes"] = MESH_MAX_NODES;
    if (macStr && *macStr) doc["mac"] = macStr;

    String out;
    serializeJson(doc, out);
    return out;
}

static uint32_t defaultCapabilitiesForType(NodeType type) {
    switch (type) {
        case NODE_SENSOR: return NODE_CAP_SENSOR_DATA;
        case NODE_ACTUATOR: return NODE_CAP_ACTUATORS;
        case NODE_HYBRID: return NODE_CAP_ACTUATORS;
        default: return 0;
    }
}

static uint8_t defaultActuatorCountForNode(const NodeRecord& node) {
    if (node.actuatorCount > 0) return node.actuatorCount;
    if (node.capabilities & NODE_CAP_ACTUATORS) return 4;
    return 0;
}

static bool nodeSupportsCapability(const NodeRecord& node, uint32_t capability) {
    return (node.capabilities & capability) != 0;
}

// *****************************************************************************
//  Web Auth helpers
// *****************************************************************************
static bool credentialsSet() {
    return webUsername[0] != '\0' && webPassHash[0] != '\0';
}

static bool isWsAuthenticated(uint32_t clientId) {
    if (!credentialsSet()) return true;
    return authWsClients.count(clientId) > 0;
}

static void computeSha256Hex(const char* input, char* hexOut64) {
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);   // 0 = SHA-256 (not SHA-224)
    mbedtls_sha256_update(&ctx, (const unsigned char*)input, strlen(input));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    for (int i = 0; i < 32; i++) snprintf(hexOut64 + i * 2, 3, "%02x", hash[i]);
    hexOut64[64] = '\0';
}

static void generateHexToken(char* out, int byteCount) {
    // out must be at least byteCount*2 + 1 bytes
    for (int i = 0; i < byteCount; i++)
        snprintf(out + i * 2, 3, "%02x", (uint8_t)(esp_random() & 0xFF));
    out[byteCount * 2] = '\0';
}

static void loadWebCredentials() {
    Preferences prefs;
    prefs.begin("gwauth", true);   // read-only
    String u = prefs.getString("username", "");
    String h = prefs.getString("passhash", "");
    String t = prefs.getString("remtoken", "");
    prefs.end();
    strncpy(webUsername,   u.c_str(), 32); webUsername[32]   = '\0';
    strncpy(webPassHash,   h.c_str(), 64); webPassHash[64]   = '\0';
    strncpy(rememberToken, t.c_str(), 64); rememberToken[64] = '\0';
    Serial.printf("[AUTH]  Web credentials %s\n",
                  credentialsSet() ? "loaded" : "not set (open access)");
}

// Saves new username + pre-hashed password; also generates a fresh remember token.
static void saveWebCredentials(const char* user, const char* passHash) {
    generateHexToken(rememberToken, 32);
    Preferences prefs;
    prefs.begin("gwauth", false);  // read-write
    prefs.putString("username", user);
    prefs.putString("passhash", passHash);
    prefs.putString("remtoken", rememberToken);
    prefs.end();
    strncpy(webUsername, user,     32); webUsername[32] = '\0';
    strncpy(webPassHash, passHash, 64); webPassHash[64] = '\0';
    Serial.printf("[AUTH]  Web credentials saved for user \"%s\"\n", user);
}

// Parses the raw "Cookie" header and returns the value for the named cookie,
// or an empty String if not found. ESPAsyncWebServer v3 has no dedicated
// cookie API — cookies live in the plain HTTP header.
static String getCookieValue(AsyncWebServerRequest* req, const char* name) {
    const AsyncWebHeader* hdr = req->getHeader("Cookie");
    if (!hdr) return String();
    const String& raw = hdr->value();
    // raw looks like: "key1=val1; key2=val2; key3=val3"
    String nameEq = String(name) + "=";
    int start = 0;
    while (start < (int)raw.length()) {
        // Skip leading spaces
        while (start < (int)raw.length() && raw[start] == ' ') start++;
        int semi = raw.indexOf(';', start);
        String pair = (semi < 0) ? raw.substring(start) : raw.substring(start, semi);
        pair.trim();
        if (pair.startsWith(nameEq)) {
            return pair.substring(nameEq.length());
        }
        if (semi < 0) break;
        start = semi + 1;
    }
    // For Debugging: print the cookie parsing attempt to the Serial console
    Serial.printf("[AUTH]  Cookie \"%s\" not found in header: %s\n", name, raw.c_str());
    return String();
}

static bool shouldLogHttpAuthSuccess(AsyncWebServerRequest* req) {
    return req &&
        req->url() != "/api/gateway/ota" &&
        req->url() != "/api/node/ota";
}

// Returns true if the HTTP request carries a valid session or remember cookie.
static bool isHttpAuthenticated(AsyncWebServerRequest* req) {
    if (!credentialsSet()) {
        // For Debugging: print the authentication attempt to the Serial console
        if (shouldLogHttpAuthSuccess(req)) {
            Serial.printf("[AUTH]  HTTP auth not required for request to %s\n", req->url().c_str());
        }
        return true;
    }
    if (strlen(sessionToken) > 0) {
        String v = getCookieValue(req, "gwsession");
        if (v.length() > 0 && v.equals(sessionToken)) {
            // For Debugging: print the authentication attempt to the Serial console
            if (shouldLogHttpAuthSuccess(req)) {
                Serial.printf("[AUTH]  HTTP auth OK for request to %s\n", req->url().c_str());
            }
            return true;
        }
    }
    if (strlen(rememberToken) > 0) {
        String v = getCookieValue(req, "gwremember");
        if (v.length() > 0 && v.equals(rememberToken)) {
            // For Debugging: print the authentication attempt to the Serial console
            if (shouldLogHttpAuthSuccess(req)) {
                Serial.printf("[AUTH]  HTTP auth OK for request to %s\n", req->url().c_str());
            }
            return true;
        }
    }
    const AsyncWebHeader* tokenHdr = req->getHeader("X-GW-Token");
    if (tokenHdr) {
        const String token = tokenHdr->value();
        if ((strlen(sessionToken) > 0 && token.equals(sessionToken)) ||
            (strlen(rememberToken) > 0 && token.equals(rememberToken))) {
            if (shouldLogHttpAuthSuccess(req)) {
                Serial.printf("[AUTH]  HTTP token auth OK for request to %s\n", req->url().c_str());
            }
            return true;
        }
    }
    // For Debugging: print the authentication attempt to the Serial console
    Serial.printf("[AUTH]  HTTP auth failed for request to %s\n", req->url().c_str());
    return false;
}

// Broadcast a message only to authenticated WS clients
// (or to all clients when no credentials are configured).
static void wsBroadcast(const String& msg) {
    if (!credentialsSet()) { ws.textAll(msg); return; }
    for (uint32_t id : authWsClients) {
        AsyncWebSocketClient* c = ws.client(id);
        if (c && c->canSend()) c->text(msg);
        // For Debugging: print the message being sent to the Serial console
        Serial.printf("[WS]    Sent message to client %d: %s\n", id, msg.c_str());
    }
}

static size_t getGatewayOtaSlotSize() {
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    return part ? part->size : 0;
}

static bool gatewayOtaSupported() {
    return getGatewayOtaSlotSize() > 0;
}

static void setOtaError(GatewayOtaRequestState* state, const String& msg) {
    if (!state || state->error[0] != '\0') return;
    strncpy(state->error, msg.c_str(), sizeof(state->error) - 1);
    state->error[sizeof(state->error) - 1] = '\0';
    Serial.printf("[OTA]  ERROR: %s\n", state->error);
}

static void scanGatewayFirmwareMarkers(GatewayOtaRequestState* state,
                                       const uint8_t* data,
                                       size_t len) {
    if (!state || !data || len == 0) return;

    for (size_t i = 0; i < len; i++) {
        const char ch = (char)data[i];

        if (state->markerReadingVersion) {
            if (ch == '\0') {
                if (state->markerVersionLen > 0) {
                    state->incomingDisplayVersion[state->markerVersionLen] = '\0';
                }
                state->markerReadingVersion = false;
                state->markerVersionLen = 0;
                state->markerMatchIndex = 0;
                continue;
            }

            if (ch >= 32 && ch <= 126 && state->markerVersionLen < (OTA_FW_VERSION_MAX_LEN - 1)) {
                state->incomingDisplayVersion[state->markerVersionLen++] = ch;
                continue;
            }

            state->markerReadingVersion = false;
            state->markerVersionLen = 0;
            state->markerMatchIndex = 0;
        }

        if (state->hwMarkerReading) {
            if (ch == '\0') {
                if (state->hwMarkerLen > 0) {
                    state->incomingHwConfigId[state->hwMarkerLen] = '\0';
                }
                state->hwMarkerReading = false;
                state->hwMarkerLen = 0;
                state->hwMarkerMatchIndex = 0;
                continue;
            }

            if (ch >= 32 && ch <= 126 && state->hwMarkerLen < (OTA_HWCFG_ID_MAX_LEN - 1)) {
                state->incomingHwConfigId[state->hwMarkerLen++] = ch;
                continue;
            }

            state->hwMarkerReading = false;
            state->hwMarkerLen = 0;
            state->hwMarkerMatchIndex = 0;
        }

        if (ch == OTA_FW_MARKER[state->markerMatchIndex]) {
            state->markerMatchIndex++;
            if (state->markerMatchIndex == OTA_FW_MARKER_LEN) {
                state->markerReadingVersion = true;
                state->markerVersionLen = 0;
                memset(state->incomingDisplayVersion, 0, sizeof(state->incomingDisplayVersion));
                state->markerMatchIndex = 0;
            }
        } else {
            state->markerMatchIndex = (ch == OTA_FW_MARKER[0]) ? 1 : 0;
        }

        if (ch == OTA_HWCFG_MARKER[state->hwMarkerMatchIndex]) {
            state->hwMarkerMatchIndex++;
            if (state->hwMarkerMatchIndex == OTA_HWCFG_MARKER_LEN) {
                state->hwMarkerReading = true;
                state->hwMarkerLen = 0;
                memset(state->incomingHwConfigId, 0, sizeof(state->incomingHwConfigId));
                state->hwMarkerMatchIndex = 0;
            }
        } else {
            state->hwMarkerMatchIndex = (ch == OTA_HWCFG_MARKER[0]) ? 1 : 0;
        }
    }
}

static const char* getIncomingGatewayFirmwareVersion(const GatewayOtaRequestState* state) {
    if (!state) return "(unknown)";
    if (state->incomingDisplayVersion[0] != '\0') return state->incomingDisplayVersion;
    if (state->incomingVersion[0] != '\0') return state->incomingVersion;
    return "(unknown)";
}

static bool parseHardwareConfigId(const char* text, uint32_t* outValue = nullptr) {
    if (!text) return false;
    while (*text != '\0' && isspace((unsigned char)*text)) text++;
    if (*text == '\0') return false;

    char* end = nullptr;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text) return false;
    while (*end != '\0' && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return false;

    if (outValue) *outValue = (uint32_t)value;
    return true;
}

static bool hardwareConfigIdsMatch(const char* lhs, const char* rhs) {
    uint32_t lhsValue = 0;
    uint32_t rhsValue = 0;
    return parseHardwareConfigId(lhs, &lhsValue) &&
           parseHardwareConfigId(rhs, &rhsValue) &&
           lhsValue == rhsValue;
}

static bool validateGatewayFirmwareDescriptor(GatewayOtaRequestState* state) {
    if (!state) return false;
    if (state->descBytes < OTA_DESC_BUF_LEN) return false;
    if (state->metadataValidated) return true;

    const esp_app_desc_t* incoming =
        reinterpret_cast<const esp_app_desc_t*>(state->descBuf
            + sizeof(esp_image_header_t)
            + sizeof(esp_image_segment_header_t));
    const esp_app_desc_t* running = esp_app_get_description();

    strncpy(state->incomingVersion, incoming->version, sizeof(state->incomingVersion) - 1);
    state->incomingVersion[sizeof(state->incomingVersion) - 1] = '\0';

    if (running &&
        running->project_name[0] != '\0' &&
        incoming->project_name[0] != '\0' &&
        strncmp(running->project_name, incoming->project_name, sizeof(running->project_name)) != 0) {
        setOtaError(state, String("Firmware project mismatch: expected \"")
            + running->project_name + "\", got \"" + incoming->project_name + "\".");
        return false;
    }

    state->metadataValidated = true;
    Serial.printf("[OTA]  Incoming firmware descriptor version: %s\n",
                  state->incomingVersion[0] ? state->incomingVersion : "(unknown)");
    return true;
}

static void handleGatewayOtaUpload(AsyncWebServerRequest* req,
                                   const String& filename,
                                   size_t index,
                                   uint8_t* data,
                                   size_t len,
                                   bool final) {
    if (!isHttpAuthenticated(req)) return;

    auto* state = reinterpret_cast<GatewayOtaRequestState*>(req->_tempObject);
    if (index == 0) {
        if (state) {
            delete state;
            state = nullptr;
        }
        state = new GatewayOtaRequestState();
        req->_tempObject = state;
        state->started = true;
        const AsyncWebHeader* sizeHdr = req->getHeader("X-Firmware-Size");
        if (sizeHdr) {
            state->expectedSize = (size_t)strtoull(sizeHdr->value().c_str(), nullptr, 10);
        }
        strncpy(state->filename, filename.c_str(), sizeof(state->filename) - 1);
        state->filename[sizeof(state->filename) - 1] = '\0';

        if (otaUploadBusy) {
            setOtaError(state, "Another OTA upload is already in progress.");
        } else if (otaRebootPending) {
            setOtaError(state, "Gateway reboot pending. Wait for the device to come back online.");
        } else if (!gatewayOtaSupported()) {
            setOtaError(state, "This firmware layout does not support OTA updates.");
        } else {
            const size_t slotSize = getGatewayOtaSlotSize();
            if (state->expectedSize > 0 && state->expectedSize > slotSize) {
                setOtaError(state, String("Firmware image is too large for the OTA slot (")
                    + state->expectedSize + " > " + slotSize + " bytes).");
            } else if (!Update.begin(slotSize, U_FLASH)) {
                setOtaError(state, String("Could not start OTA: ") + Update.errorString());
            } else {
                otaUploadBusy = true;
                state->updateBegun = true;
                if (state->expectedSize > 0) {
                    Serial.printf("[OTA]  Upload started: \"%s\" (%u bytes)\n",
                                  state->filename[0] ? state->filename : "(unnamed)",
                                  (unsigned)state->expectedSize);
                } else {
                    Serial.printf("[OTA]  Upload started: \"%s\" (size unknown)\n",
                                  state->filename[0] ? state->filename : "(unnamed)");
                }
            }
        }
    }

    if (!state) return;

    if (state->error[0] != '\0') {
        if (final && state->updateBegun) {
            Update.abort();
            state->updateBegun = false;
        }
        return;
    }

    if (len > 0) {
        scanGatewayFirmwareMarkers(state, data, len);

        if (index == 0 && data[0] != ESP_IMAGE_HEADER_MAGIC) {
            setOtaError(state, "Uploaded file is not a valid ESP32 application image.");
        }

        if (state->error[0] == '\0' && !state->metadataValidated) {
            const size_t copyLen = std::min<size_t>(len, OTA_DESC_BUF_LEN - state->descBytes);
            memcpy(state->descBuf + state->descBytes, data, copyLen);
            state->descBytes += copyLen;

            if (state->descBytes >= OTA_DESC_BUF_LEN) {
                validateGatewayFirmwareDescriptor(state);
                if (state->error[0] != '\0' && state->updateBegun) {
                    Update.abort();
                    state->updateBegun = false;
                } else if (!state->descFlushed) {
                    size_t flushed = Update.write(state->descBuf, OTA_DESC_BUF_LEN);
                    if (flushed != OTA_DESC_BUF_LEN) {
                        setOtaError(state, String("Write failed: ") + Update.errorString());
                        if (state->updateBegun) {
                            Update.abort();
                            state->updateBegun = false;
                        }
                    } else {
                        state->written += flushed;
                        state->descFlushed = true;
                    }
                }
            }

            if (state->error[0] == '\0' && state->metadataValidated) {
                const size_t trailingLen = len - copyLen;
                if (trailingLen > 0) {
                    size_t flushed = Update.write(data + copyLen, trailingLen);
                    if (flushed != trailingLen) {
                        setOtaError(state, String("Write failed: ") + Update.errorString());
                        if (state->updateBegun) {
                            Update.abort();
                            state->updateBegun = false;
                        }
                    } else {
                        state->written += flushed;
                    }
                }
            }
        } else if (state->error[0] == '\0') {
            size_t written = Update.write(data, len);
            if (written != len) {
                setOtaError(state, String("Write failed: ") + Update.errorString());
                if (state->updateBegun) {
                    Update.abort();
                    state->updateBegun = false;
                }
            } else {
                state->written += written;
            }
        }
    }

    if (final && state->error[0] == '\0') {
        if (state->incomingDisplayVersion[0] == '\0' && state->incomingVersion[0] != '\0') {
            strncpy(state->incomingDisplayVersion, state->incomingVersion, sizeof(state->incomingDisplayVersion) - 1);
            state->incomingDisplayVersion[sizeof(state->incomingDisplayVersion) - 1] = '\0';
        }

        if (!state->metadataValidated) {
            setOtaError(state, "Firmware metadata could not be validated.");
        } else if (state->incomingHwConfigId[0] == '\0') {
            setOtaError(state, "Gateway firmware hardware configuration marker (GWHWCFG) is missing. Rebuild the uploaded gateway firmware with the current OTA markers.");
        } else if (!hardwareConfigIdsMatch(state->incomingHwConfigId, HW_CONFIG_ID)) {
            setOtaError(state, String("Gateway hardware configuration mismatch. This gateway supports hardware config ID ")
                + HW_CONFIG_ID + " but the uploaded firmware targets " + state->incomingHwConfigId + ".");
        } else if (state->expectedSize > 0 && state->written != state->expectedSize) {
            setOtaError(state, String("Upload size mismatch: wrote ")
                + state->written + " of " + state->expectedSize + " bytes.");
        } else if (state->written == 0) {
            setOtaError(state, "Empty firmware upload received.");
        } else if (!Update.end(true)) {
            setOtaError(state, String("Firmware finalization failed: ") + Update.errorString());
        } else if (!Update.isFinished()) {
            setOtaError(state, "Firmware upload did not complete.");
        } else {
            state->ok = true;
            Serial.printf("[OTA]  Upload complete. Ready to reboot into version %s.\n",
                          getIncomingGatewayFirmwareVersion(state));
        }
    }

    if (final && !state->ok && state->updateBegun) {
        Update.abort();
        state->updateBegun = false;
    }
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

static const char* nodeTypeToRole(NodeType type) {
    switch (type) {
        case NODE_SENSOR: return "SENSOR";
        case NODE_ACTUATOR: return "ACTUATOR";
        case NODE_HYBRID: return "HYBRID";
        default: return "UNKNOWN";
    }
}

static NodeType roleToNodeType(const char* role) {
    if (!role || role[0] == '\0') return (NodeType)0;
    if (strcmp(role, "SENSOR") == 0) return NODE_SENSOR;
    if (strcmp(role, "ACTUATOR") == 0) return NODE_ACTUATOR;
    if (strcmp(role, "HYBRID") == 0) return NODE_HYBRID;
    return (NodeType)0;
}

static void setNodeOtaUploadError(NodeOtaUploadRequestState* state, const String& msg) {
    if (!state || state->error[0] != '\0') return;
    strncpy(state->error, msg.c_str(), sizeof(state->error) - 1);
    state->error[sizeof(state->error) - 1] = '\0';
    Serial.printf("[NODE OTA]  ERROR: %s\n", state->error);
}

static void scanNodeFirmwareMarkers(NodeOtaUploadRequestState* state,
                                    const uint8_t* data,
                                    size_t len) {
    if (!state || !data || len == 0) return;

    for (size_t i = 0; i < len; i++) {
        const char ch = (char)data[i];

        if (state->versionMarkerReading) {
            if (ch == '\0') {
                if (state->versionMarkerLen > 0) {
                    state->incomingDisplayVersion[state->versionMarkerLen] = '\0';
                }
                state->versionMarkerReading = false;
                state->versionMarkerLen = 0;
            } else if (ch >= 32 && ch <= 126 &&
                       state->versionMarkerLen < (NODE_OTA_VERSION_MAX_LEN - 1)) {
                state->incomingDisplayVersion[state->versionMarkerLen++] = ch;
            } else {
                state->versionMarkerReading = false;
                state->versionMarkerLen = 0;
            }
        }

        if (state->roleMarkerReading) {
            if (ch == '\0') {
                if (state->roleMarkerLen > 0) state->incomingRole[state->roleMarkerLen] = '\0';
                state->roleMarkerReading = false;
                state->roleMarkerLen = 0;
            } else if (ch >= 'A' && ch <= 'Z' &&
                       state->roleMarkerLen < (sizeof(state->incomingRole) - 1)) {
                state->incomingRole[state->roleMarkerLen++] = ch;
            } else {
                state->roleMarkerReading = false;
                state->roleMarkerLen = 0;
            }
        }

        if (state->hwMarkerReading) {
            if (ch == '\0') {
                if (state->hwMarkerLen > 0) state->incomingHwConfigId[state->hwMarkerLen] = '\0';
                state->hwMarkerReading = false;
                state->hwMarkerLen = 0;
            } else if (ch >= 32 && ch <= 126 &&
                       state->hwMarkerLen < (sizeof(state->incomingHwConfigId) - 1)) {
                state->incomingHwConfigId[state->hwMarkerLen++] = ch;
            } else {
                state->hwMarkerReading = false;
                state->hwMarkerLen = 0;
            }
        }

        if (!state->versionMarkerReading) {
            if (ch == NODE_OTA_FW_MARKER[state->versionMarkerMatchIndex]) {
                state->versionMarkerMatchIndex++;
                if (state->versionMarkerMatchIndex == NODE_OTA_FW_MARKER_LEN) {
                    state->versionMarkerReading = true;
                    state->versionMarkerLen = 0;
                    memset(state->incomingDisplayVersion, 0, sizeof(state->incomingDisplayVersion));
                    state->versionMarkerMatchIndex = 0;
                }
            } else {
                state->versionMarkerMatchIndex = (ch == NODE_OTA_FW_MARKER[0]) ? 1 : 0;
            }
        }

        if (!state->roleMarkerReading) {
            if (ch == NODE_OTA_ROLE_MARKER[state->roleMarkerMatchIndex]) {
                state->roleMarkerMatchIndex++;
                if (state->roleMarkerMatchIndex == NODE_OTA_ROLE_MARKER_LEN) {
                    state->roleMarkerReading = true;
                    state->roleMarkerLen = 0;
                    memset(state->incomingRole, 0, sizeof(state->incomingRole));
                    state->roleMarkerMatchIndex = 0;
                }
            } else {
                state->roleMarkerMatchIndex = (ch == NODE_OTA_ROLE_MARKER[0]) ? 1 : 0;
            }
        }

        if (!state->hwMarkerReading) {
            if (ch == NODE_OTA_HWCFG_MARKER[state->hwMarkerMatchIndex]) {
                state->hwMarkerMatchIndex++;
                if (state->hwMarkerMatchIndex == NODE_OTA_HWCFG_MARKER_LEN) {
                    state->hwMarkerReading = true;
                    state->hwMarkerLen = 0;
                    memset(state->incomingHwConfigId, 0, sizeof(state->incomingHwConfigId));
                    state->hwMarkerMatchIndex = 0;
                }
            } else {
                state->hwMarkerMatchIndex = (ch == NODE_OTA_HWCFG_MARKER[0]) ? 1 : 0;
            }
        }
    }
}

static String buildMetaJson();  // forward declaration

static void broadcastMetaIfClientsConnected() {
    if (ws.count() > 0) wsBroadcast(buildMetaJson());
}

static bool validateNodeFirmwareDescriptor(NodeOtaUploadRequestState* state) {
    if (!state) return false;
    if (state->descBytes < OTA_DESC_BUF_LEN) return false;
    if (state->metadataValidated) return true;

    const esp_app_desc_t* incoming =
        reinterpret_cast<const esp_app_desc_t*>(state->descBuf
            + sizeof(esp_image_header_t)
            + sizeof(esp_image_segment_header_t));

    strncpy(state->incomingVersion, incoming->version, sizeof(state->incomingVersion) - 1);
    state->incomingVersion[sizeof(state->incomingVersion) - 1] = '\0';
    state->metadataValidated = true;

    Serial.printf("[NODE OTA]  Incoming firmware descriptor version: %s\n",
                  state->incomingVersion[0] ? state->incomingVersion : "(unknown)");
    return true;
}

static String buildNodeOtaJson() {
    JsonDocument doc;
    doc["type"] = "node_ota_state";
    doc["active"] = nodeOtaJob.active;
    doc["helper_online"] = coprocOnline;
    doc["upload_busy"] = nodeOtaJob.uploadBusy;
    doc["node_id"] = nodeOtaJob.nodeId;
    doc["stage"] = (int)nodeOtaJob.stage;
    doc["progress"] = nodeOtaJob.progress;
    doc["message"] = nodeOtaJob.message;
    doc["error"] = nodeOtaJob.error;
    doc["version"] = nodeOtaJob.version;
    if (nodeOtaJob.nodeId > 0 && nodeOtaJob.nodeId < nextId) {
        doc["node_name"] = nodes[nodeOtaJob.nodeId].name;
        doc["node_mac"] = macToStr(nodes[nodeOtaJob.nodeId].mac);
    }
    String out;
    serializeJson(doc, out);
    return out;
}

static void broadcastNodeOtaState() {
    wsBroadcast(buildNodeOtaJson());
}

static void clearNodeOtaJob(bool removeFile = true) {
    if (nodeOtaJob.file) nodeOtaJob.file.close();
    if (removeFile && LittleFS.exists(NODE_OTA_FILE_PATH)) {
        LittleFS.remove(NODE_OTA_FILE_PATH);
    }
    nodeOtaJob = NodeOtaJobState{};
}

static void setNodeOtaJobMessage(const String& msg, uint8_t progress = 0) {
    nodeOtaJob.lastActivityAt = millis();
    const bool messageChanged = strncmp(nodeOtaJob.message, msg.c_str(), sizeof(nodeOtaJob.message)) != 0;
    const bool progressChanged = nodeOtaJob.progress != progress;
    if (messageChanged) {
        strncpy(nodeOtaJob.message, msg.c_str(), sizeof(nodeOtaJob.message) - 1);
        nodeOtaJob.message[sizeof(nodeOtaJob.message) - 1] = '\0';
        Serial.printf("[NODE OTA]  %s\n", nodeOtaJob.message);
    }
    if (messageChanged || progressChanged) {
        nodeOtaJob.progress = progress;
        nodeOtaJob.stageStartedAt = millis();
        broadcastNodeOtaState();
    }
}

static void setNodeOtaJobError(const String& msg) {
    nodeOtaJob.stage = NODE_OTA_JOB_ERROR;
    nodeOtaJob.awaitingAck = false;
    nodeOtaJob.awaitingFrameType = 0;
    if (strncmp(nodeOtaJob.error, msg.c_str(), sizeof(nodeOtaJob.error)) != 0) {
        strncpy(nodeOtaJob.error, msg.c_str(), sizeof(nodeOtaJob.error) - 1);
        nodeOtaJob.error[sizeof(nodeOtaJob.error) - 1] = '\0';
    }
    setNodeOtaJobMessage(msg, nodeOtaJob.progress);
}

static bool isRecoverableNodeOtaTimeout() {
    return nodeOtaJob.stage == NODE_OTA_JOB_ERROR &&
           strncmp(nodeOtaJob.error, "Node OTA timed out.", sizeof(nodeOtaJob.error)) == 0;
}

static void randomAscii(char* out, size_t len, const char* alphabet) {
    if (!out || len == 0) return;
    const size_t alphaLen = strlen(alphabet);
    if (alphaLen == 0) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i + 1 < len; i++) {
        out[i] = alphabet[esp_random() % alphaLen];
    }
    out[len - 1] = '\0';
}

static void buildNodeOtaCredentials() {
    const char* alnum = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    snprintf(nodeOtaJob.apSsid, sizeof(nodeOtaJob.apSsid), "NodeOTA-%02X%02X",
             (unsigned)(nodeOtaJob.sessionId >> 8) & 0xFF,
             (unsigned)(nodeOtaJob.sessionId) & 0xFF);
    randomAscii(nodeOtaJob.apPassword, sizeof(nodeOtaJob.apPassword), alnum);
}

static void requestCoprocReboot() {
    digitalWrite(COPROC_RESET_PIN, HIGH);
    delay(80);
    digitalWrite(COPROC_RESET_PIN, LOW);
    lastCoprocRecoveryPulseAt = millis();
    Serial.println("[C3]  Reboot pulse sent to coprocessor.");
}

static void resetCoprocRx() {
    coprocRx = CoprocRxState{};
}

static void resetCoprocAck() {
    coprocAck = CoprocAckState{};
}

static void flushCoprocSerialInput() {
    while (coprocSerial.available() > 0) {
        coprocSerial.read();
    }
    resetCoprocRx();
    resetCoprocAck();
}

static bool sendCoprocFrame(uint8_t type, const void* payload, size_t len) {
    if (len > COPROC_FRAME_MAX_PAYLOAD) return false;
    uint8_t headerBuf[sizeof(CoprocFrameHeader)];
    CoprocFrameHeader hdr{COPROC_FRAME_MAGIC, type, (uint16_t)len};
    memcpy(headerBuf, &hdr, sizeof(hdr));

    uint8_t crcBuf[2];
    uint8_t crcData[1 + 2 + COPROC_FRAME_MAX_PAYLOAD] = {0};
    crcData[0] = type;
    crcData[1] = (uint8_t)(len & 0xFF);
    crcData[2] = (uint8_t)((len >> 8) & 0xFF);
    if (len > 0 && payload) memcpy(crcData + 3, payload, len);
    uint16_t crc = crc16Ccitt(crcData, len + 3);
    crcBuf[0] = (uint8_t)(crc & 0xFF);
    crcBuf[1] = (uint8_t)((crc >> 8) & 0xFF);

    size_t written = coprocSerial.write(headerBuf, sizeof(headerBuf));
    if (len > 0 && payload) written += coprocSerial.write((const uint8_t*)payload, len);
    written += coprocSerial.write(crcBuf, sizeof(crcBuf));
    coprocSerial.flush();
    return written == (sizeof(headerBuf) + len + sizeof(crcBuf));
}

static void handleCoprocFrame(uint8_t type, const uint8_t* payload, size_t len) {
    if (type == COPROC_FRAME_ACK) {
        if (len < sizeof(CoprocAckPayload)) return;
        const auto* ack = reinterpret_cast<const CoprocAckPayload*>(payload);
        if (nodeOtaJob.active && nodeOtaJob.awaitingAck &&
            nodeOtaJob.awaitingFrameType != COPROC_FRAME_HELLO &&
            ack->original_type == COPROC_FRAME_HELLO) {
            coprocOnline = ack->ok != 0;
            lastCoprocAckAt = millis();
            return;
        }
        coprocAck.ready = true;
        coprocAck.originalType = ack->original_type;
        coprocAck.ok = ack->ok != 0;
        coprocAck.value = ack->value;
        memcpy(coprocAck.message, ack->message, sizeof(coprocAck.message));
        nodeOtaJob.lastAckAt = millis();
        if (ack->original_type != COPROC_FRAME_UPLOAD_CHUNK) {
            Serial.printf("[C3]  ACK type=0x%02X ok=%u value=%lu msg=\"%s\"\n",
                          ack->original_type,
                          ack->ok,
                          (unsigned long)ack->value,
                          ack->message);
        }
        return;
    }

    if (type == COPROC_FRAME_STATUS) {
        if (len < sizeof(CoprocStatusPayload)) return;
        const auto* status = reinterpret_cast<const CoprocStatusPayload*>(payload);
        bool recoveredFromTimeout = false;
        Serial.printf("[C3]  STATUS session=%lu phase=%u progress=%u err=%u msg=\"%s\"\n",
                      (unsigned long)status->session_id,
                      status->phase,
                      status->progress,
                      status->error_code,
                      status->message);
        if (nodeOtaJob.active && status->session_id == nodeOtaJob.sessionId) {
            nodeOtaJob.lastActivityAt = millis();
            if (status->phase == COPROC_HELPER_AP_READY) nodeOtaJob.helperApReady = true;
            if (status->phase == COPROC_HELPER_DONE || status->phase == NODE_OTA_SUCCESS) {
                nodeOtaJob.helperTransferDone = true;
                if (nodeOtaJob.stage == NODE_OTA_JOB_ERROR &&
                    strncmp(nodeOtaJob.error, "Node OTA timed out.", sizeof(nodeOtaJob.error)) == 0) {
                    nodeOtaJob.stage = NODE_OTA_JOB_WAIT_REJOIN;
                    recoveredFromTimeout = true;
                }
            }
            if (status->phase == COPROC_HELPER_ERROR || status->phase == NODE_OTA_ERROR) {
                setNodeOtaJobError(status->message[0] ? status->message : "Coprocessor reported an OTA error.");
            } else if (recoveredFromTimeout) {
                setNodeOtaJobMessage("Firmware sent to node. Waiting for node to reconnect...", 95);
            } else {
                setNodeOtaJobMessage(status->message[0] ? status->message : "OTA helper status updated.",
                                     status->progress);
            }
        }
    }
}

static void processCoprocSerial() {
    while (coprocSerial.available() > 0) {
        const uint8_t b = (uint8_t)coprocSerial.read();
        switch (coprocRx.state) {
            case CoprocRxState::WAIT_MAGIC_1:
                if (b == (COPROC_FRAME_MAGIC & 0xFF)) coprocRx.state = CoprocRxState::WAIT_MAGIC_2;
                break;
            case CoprocRxState::WAIT_MAGIC_2:
                if (b == ((COPROC_FRAME_MAGIC >> 8) & 0xFF)) coprocRx.state = CoprocRxState::READ_TYPE;
                else coprocRx.state = CoprocRxState::WAIT_MAGIC_1;
                break;
            case CoprocRxState::READ_TYPE:
                coprocRx.type = b;
                coprocRx.state = CoprocRxState::READ_LEN_1;
                break;
            case CoprocRxState::READ_LEN_1:
                coprocRx.length = b;
                coprocRx.state = CoprocRxState::READ_LEN_2;
                break;
            case CoprocRxState::READ_LEN_2:
                coprocRx.length |= (uint16_t)b << 8;
                if (coprocRx.length > COPROC_FRAME_MAX_PAYLOAD) {
                    resetCoprocRx();
                } else if (coprocRx.length == 0) {
                    coprocRx.state = CoprocRxState::READ_CRC_1;
                } else {
                    coprocRx.payloadIndex = 0;
                    coprocRx.state = CoprocRxState::READ_PAYLOAD;
                }
                break;
            case CoprocRxState::READ_PAYLOAD:
                coprocRx.payload[coprocRx.payloadIndex++] = b;
                if (coprocRx.payloadIndex >= coprocRx.length) {
                    coprocRx.state = CoprocRxState::READ_CRC_1;
                }
                break;
            case CoprocRxState::READ_CRC_1:
                coprocRx.expectedCrc = b;
                coprocRx.state = CoprocRxState::READ_CRC_2;
                break;
            case CoprocRxState::READ_CRC_2: {
                coprocRx.expectedCrc |= (uint16_t)b << 8;
                uint8_t crcData[1 + 2 + COPROC_FRAME_MAX_PAYLOAD] = {0};
                crcData[0] = coprocRx.type;
                crcData[1] = (uint8_t)(coprocRx.length & 0xFF);
                crcData[2] = (uint8_t)((coprocRx.length >> 8) & 0xFF);
                if (coprocRx.length > 0) memcpy(crcData + 3, coprocRx.payload, coprocRx.length);
                const uint16_t actual = crc16Ccitt(crcData, coprocRx.length + 3);
                if (actual == coprocRx.expectedCrc) {
                    handleCoprocFrame(coprocRx.type, coprocRx.payload, coprocRx.length);
                }
                resetCoprocRx();
                break;
            }
        }
    }
}

static bool sendCoprocHello() {
    resetCoprocAck();
    Serial.println("[C3]  Sending HELLO to coprocessor...");
    const bool ok = sendCoprocFrame(COPROC_FRAME_HELLO, nullptr, 0);
    lastCoprocHelloAt = millis();
    return ok;
}

static void beginCoprocAwait(uint8_t frameType, unsigned long now, bool resetRetries = true) {
    nodeOtaJob.awaitingAck = true;
    nodeOtaJob.awaitingFrameType = frameType;
    nodeOtaJob.lastAckAt = now;
    if (resetRetries) nodeOtaJob.coprocAckRetries = 0;
}

static bool sendCoprocUploadBeginFrame(unsigned long now, bool resetRetries = true) {
    CoprocUploadBeginPayload payload{};
    payload.session_id = nodeOtaJob.sessionId;
    payload.image_size = (uint32_t)nodeOtaJob.imageSize;
    payload.image_crc32 = nodeOtaJob.imageCrc32;
    payload.node_type = (uint8_t)nodeOtaJob.targetType;
    payload.ap_channel = NODE_OTA_AP_CHANNEL;
    payload.port = NODE_OTA_PORT;
    strncpy(payload.version, nodeOtaJob.version, sizeof(payload.version) - 1);
    strncpy(payload.ssid, nodeOtaJob.apSsid, sizeof(payload.ssid) - 1);
    strncpy(payload.password, nodeOtaJob.apPassword, sizeof(payload.password) - 1);
    resetCoprocAck();
    if (!sendCoprocFrame(COPROC_FRAME_UPLOAD_BEGIN, &payload, sizeof(payload))) {
        return false;
    }
    beginCoprocAwait(COPROC_FRAME_UPLOAD_BEGIN, now, resetRetries);
    return true;
}

static bool sendCoprocUploadChunkFrame(unsigned long now, bool resetRetries = true, bool* reachedEnd = nullptr) {
    if (reachedEnd) *reachedEnd = false;
    if (!nodeOtaJob.file) {
        nodeOtaJob.file = LittleFS.open(NODE_OTA_FILE_PATH, "r");
        if (!nodeOtaJob.file) {
            return false;
        }
    }

    CoprocUploadChunkPayload chunk{};
    chunk.offset = (uint32_t)nodeOtaJob.uploadOffset;
    if (!nodeOtaJob.file.seek(nodeOtaJob.uploadOffset, SeekSet)) {
        return false;
    }

    const size_t n = nodeOtaJob.file.read(chunk.data, sizeof(chunk.data));
    if (n == 0) {
        if (reachedEnd) *reachedEnd = true;
        return true;
    }

    resetCoprocAck();
    if (!sendCoprocFrame(COPROC_FRAME_UPLOAD_CHUNK, &chunk, sizeof(chunk.offset) + n)) {
        return false;
    }
    beginCoprocAwait(COPROC_FRAME_UPLOAD_CHUNK, now, resetRetries);
    return true;
}

static bool sendCoprocUploadEndFrame(unsigned long now, bool resetRetries = true) {
    resetCoprocAck();
    if (!sendCoprocFrame(COPROC_FRAME_UPLOAD_END, &nodeOtaJob.sessionId, sizeof(nodeOtaJob.sessionId))) {
        return false;
    }
    beginCoprocAwait(COPROC_FRAME_UPLOAD_END, now, resetRetries);
    return true;
}

static void initCoprocLink() {
    pinMode(COPROC_RESET_PIN, OUTPUT);
    digitalWrite(COPROC_RESET_PIN, LOW);
    coprocSerial.setRxBufferSize(COPROC_UART_RX_BUFFER_SIZE);
    coprocSerial.setTxBufferSize(COPROC_UART_TX_BUFFER_SIZE);
    coprocSerial.begin(COPROC_UART_BAUD, SERIAL_8N1, COPROC_UART_RX_PIN, COPROC_UART_TX_PIN);
    Serial.printf("[C3]  Coprocessor UART initialized: baud=%u RX=%u TX=%u reset=%u rxbuf=%u txbuf=%u\n",
                  (unsigned)COPROC_UART_BAUD,
                  (unsigned)COPROC_UART_RX_PIN,
                  (unsigned)COPROC_UART_TX_PIN,
                  (unsigned)COPROC_RESET_PIN,
                  (unsigned)COPROC_UART_RX_BUFFER_SIZE,
                  (unsigned)COPROC_UART_TX_BUFFER_SIZE);
    flushCoprocSerialInput();
    sendCoprocHello();
}

static void processCoprocHeartbeat(unsigned long now) {
    const unsigned long pingInterval = coprocOnline ? COPROC_LINK_PING_MS : 1500UL;
    if (!nodeOtaJob.active && (now - lastCoprocHelloAt) >= pingInterval) {
        sendCoprocHello();
    }

    if (coprocAck.ready && coprocAck.originalType == COPROC_FRAME_HELLO) {
        const bool wasOnline = coprocOnline;
        coprocOnline = coprocAck.ok;
        lastCoprocAckAt = now;
        lastCoprocNoAckLogAt = 0;
        if (coprocOnline) lastCoprocRecoveryPulseAt = 0;
        if (coprocOnline && !wasOnline) {
            Serial.println("[C3]  Coprocessor helper online.");
            broadcastMetaIfClientsConnected();
        }
        resetCoprocAck();
    }

    if (!coprocOnline && lastCoprocHelloAt > 0 && (now - lastCoprocHelloAt) > COPROC_ACK_TIMEOUT_MS) {
        if (lastCoprocNoAckLogAt == 0 || (now - lastCoprocNoAckLogAt) > 5000UL) {
            Serial.println("[C3]  No HELLO response from coprocessor yet.");
            lastCoprocNoAckLogAt = now;
        }

        const unsigned long recoveryAge = (lastCoprocAckAt > 0) ? (now - lastCoprocAckAt) : (now - lastCoprocHelloAt);
        if (!nodeOtaJob.active &&
            recoveryAge >= COPROC_RECOVERY_REBOOT_MS &&
            (lastCoprocRecoveryPulseAt == 0 ||
             (now - lastCoprocRecoveryPulseAt) >= COPROC_RECOVERY_COOLDOWN_MS)) {
            Serial.println("[C3]  Coprocessor still offline. Requesting helper reboot and resetting the UART parser.");
            flushCoprocSerialInput();
            requestCoprocReboot();
            lastCoprocHelloAt = now;
        }
    }

    if (coprocOnline && !nodeOtaJob.active && lastCoprocAckAt > 0 &&
        (now - lastCoprocAckAt) > (COPROC_LINK_PING_MS * 3)) {
        coprocOnline = false;
        Serial.println("[C3]  Coprocessor helper heartbeat lost.");
        broadcastMetaIfClientsConnected();
    }
}

static bool sendNodeOtaBegin(uint8_t nodeId, uint8_t attempt = 1) {
    if (nodeId == 0 || nodeId >= nextId) return false;
    if (nodes[nodeId].mac[0] == 0) return false;

    MsgNodeOtaBegin msg{};
    msg.hdr.type = MSG_NODE_OTA_BEGIN;
    msg.hdr.node_id = nodeId;
    msg.hdr.node_type = nodes[nodeId].type;
    msg.session_id = nodeOtaJob.sessionId;
    msg.image_size = (uint32_t)nodeOtaJob.imageSize;
    msg.image_crc32 = nodeOtaJob.imageCrc32;
    msg.port = NODE_OTA_PORT;
    strncpy(msg.ssid, nodeOtaJob.apSsid, sizeof(msg.ssid) - 1);
    strncpy(msg.password, nodeOtaJob.apPassword, sizeof(msg.password) - 1);
    strncpy(msg.version, nodeOtaJob.version, sizeof(msg.version) - 1);

    const esp_err_t err = esp_now_send(nodes[nodeId].mac, reinterpret_cast<uint8_t*>(&msg), sizeof(msg));
    if (err != ESP_OK) {
        Serial.printf("[NODE OTA]  Failed to send OTA begin to node #%u on attempt %u\n", nodeId, attempt);
        return false;
    }

    Serial.printf("[NODE OTA]  Sent OTA begin to node #%u (%s) attempt %u\n",
                  nodeId, macToStr(nodes[nodeId].mac).c_str(), attempt);
    return true;
}

static void processNodeOtaJob(unsigned long now) {
    if (!nodeOtaJob.active) return;

    if (nodeOtaJob.awaitingAck) {
        if (coprocAck.ready && coprocAck.originalType == nodeOtaJob.awaitingFrameType) {
            const bool ok = coprocAck.ok;
            const uint8_t originalType = coprocAck.originalType;
            const uint32_t value = coprocAck.value;
            char msg[COPROC_STATUS_MESSAGE_LEN];
            memcpy(msg, coprocAck.message, sizeof(msg));
            resetCoprocAck();
            nodeOtaJob.awaitingAck = false;
            nodeOtaJob.lastActivityAt = now;
            nodeOtaJob.coprocAckRetries = 0;

            if (!ok) {
                setNodeOtaJobError(msg[0] ? msg : "Coprocessor rejected the OTA command.");
                return;
            }

            if (originalType == COPROC_FRAME_HELLO) {
                coprocOnline = true;
            } else if (originalType == COPROC_FRAME_UPLOAD_BEGIN) {
                nodeOtaJob.stage = NODE_OTA_JOB_COPROC_STREAM;
                setNodeOtaJobMessage("Streaming firmware to OTA coprocessor...", 25);
            } else if (originalType == COPROC_FRAME_UPLOAD_CHUNK) {
                nodeOtaJob.uploadOffset = value;
                const uint8_t progress = (uint8_t)std::min<size_t>(60,
                    25 + ((nodeOtaJob.uploadOffset * 35) / std::max<size_t>(1, nodeOtaJob.imageSize)));
                setNodeOtaJobMessage("Streaming firmware to OTA coprocessor...", progress);
            } else if (originalType == COPROC_FRAME_UPLOAD_END) {
                nodeOtaJob.helperApReady = true;
                nodeOtaJob.stage = NODE_OTA_JOB_SEND_COMMAND;
                setNodeOtaJobMessage("OTA helper ready. Telling node to start update...", 65);
            }
            return;
        } else if ((now - nodeOtaJob.lastAckAt) > COPROC_ACK_TIMEOUT_MS) {
            if (nodeOtaJob.coprocAckRetries < COPROC_ACK_RETRY_LIMIT) {
                nodeOtaJob.coprocAckRetries++;
                Serial.printf("[NODE OTA]  Retrying coprocessor frame 0x%02X (%u/%u)\n",
                              nodeOtaJob.awaitingFrameType,
                              nodeOtaJob.coprocAckRetries,
                              (unsigned)COPROC_ACK_RETRY_LIMIT);
                bool resendOk = false;
                if (nodeOtaJob.awaitingFrameType == COPROC_FRAME_HELLO) {
                    resendOk = sendCoprocHello();
                    if (resendOk) beginCoprocAwait(COPROC_FRAME_HELLO, now, false);
                } else if (nodeOtaJob.awaitingFrameType == COPROC_FRAME_UPLOAD_BEGIN) {
                    resendOk = sendCoprocUploadBeginFrame(now, false);
                } else if (nodeOtaJob.awaitingFrameType == COPROC_FRAME_UPLOAD_CHUNK) {
                    bool reachedEnd = false;
                    resendOk = sendCoprocUploadChunkFrame(now, false, &reachedEnd);
                    if (reachedEnd) {
                        setNodeOtaJobError("Unexpected end of staged firmware while retrying chunk upload.");
                        return;
                    }
                } else if (nodeOtaJob.awaitingFrameType == COPROC_FRAME_UPLOAD_END) {
                    resendOk = sendCoprocUploadEndFrame(now, false);
                }
                if (!resendOk) {
                    setNodeOtaJobError("Failed to resend the OTA frame to the coprocessor.");
                    return;
                }
                nodeOtaJob.lastActivityAt = now;
                return;
            }
            String timeoutMsg = String("No response from OTA coprocessor");
            if (nodeOtaJob.awaitingFrameType == COPROC_FRAME_UPLOAD_BEGIN) {
                timeoutMsg += " during upload begin.";
            } else if (nodeOtaJob.awaitingFrameType == COPROC_FRAME_UPLOAD_CHUNK) {
                timeoutMsg += String(" during chunk upload at offset ") + nodeOtaJob.uploadOffset + ".";
            } else if (nodeOtaJob.awaitingFrameType == COPROC_FRAME_UPLOAD_END) {
                timeoutMsg += " while starting the OTA AP.";
            } else if (nodeOtaJob.awaitingFrameType == COPROC_FRAME_HELLO) {
                timeoutMsg += " during helper handshake.";
            } else {
                timeoutMsg += ".";
            }
            setNodeOtaJobError(timeoutMsg);
            return;
        } else {
            return;
        }
    }

    if (nodeOtaJob.stage == NODE_OTA_JOB_ERROR || nodeOtaJob.stage == NODE_OTA_JOB_SUCCESS) {
        unsigned long clearDelayMs = 5000UL;
        if (isRecoverableNodeOtaTimeout()) {
            clearDelayMs = NODE_OTA_TIMEOUT_RECOVERY_MS;
        }
        if ((now - nodeOtaJob.stageStartedAt) > clearDelayMs) {
            sendCoprocFrame(COPROC_FRAME_ABORT, &nodeOtaJob.sessionId, sizeof(nodeOtaJob.sessionId));
            clearNodeOtaJob(true);
        }
        return;
    }

    if (nodeOtaJob.stage == NODE_OTA_JOB_WAIT_REJOIN &&
        (now - nodeOtaJob.stageStartedAt) > NODE_OTA_REJOIN_TIMEOUT_MS) {
        setNodeOtaJobError("Node did not come back online after flashing.");
        return;
    }

    const bool coprocTransferStage =
        nodeOtaJob.stage == NODE_OTA_JOB_COPROC_WAIT ||
        nodeOtaJob.stage == NODE_OTA_JOB_COPROC_BEGIN ||
        nodeOtaJob.stage == NODE_OTA_JOB_COPROC_STREAM ||
        nodeOtaJob.stage == NODE_OTA_JOB_COPROC_FINALIZE;

    unsigned long maxIdleMs = NODE_OTA_STAGE_TIMEOUT_MS;
    if (nodeOtaJob.stage == NODE_OTA_JOB_WAIT_TRANSFER && nodeOtaJob.nodeAccepted) {
        maxIdleMs = NODE_OTA_TRANSFER_TIMEOUT_MS;
    }

    if (!coprocTransferStage &&
        nodeOtaJob.stage != NODE_OTA_JOB_WAIT_REJOIN &&
        nodeOtaJob.stage != NODE_OTA_JOB_WAIT_TRANSFER &&
        nodeOtaJob.lastActivityAt > 0 &&
        (now - nodeOtaJob.lastActivityAt) > maxIdleMs) {
        setNodeOtaJobError("Node OTA timed out.");
        return;
    }

    switch (nodeOtaJob.stage) {
        case NODE_OTA_JOB_STAGED:
            if (!coprocOnline) {
                requestCoprocReboot();
                if (!sendCoprocHello()) {
                    setNodeOtaJobError("Failed to send HELLO to the OTA coprocessor.");
                    return;
                }
                beginCoprocAwait(COPROC_FRAME_HELLO, now);
                nodeOtaJob.stage = NODE_OTA_JOB_COPROC_WAIT;
                setNodeOtaJobMessage("Waiting for OTA coprocessor...", 10);
                break;
            }
            nodeOtaJob.stage = NODE_OTA_JOB_COPROC_BEGIN;
            [[fallthrough]];

        case NODE_OTA_JOB_COPROC_BEGIN: {
            if (!sendCoprocUploadBeginFrame(now)) {
                setNodeOtaJobError("Failed to send upload begin to the OTA coprocessor.");
                return;
            }
            setNodeOtaJobMessage("Preparing OTA coprocessor storage...", 20);
            break;
        }

        case NODE_OTA_JOB_COPROC_STREAM: {
            bool reachedEnd = false;
            if (!sendCoprocUploadChunkFrame(now, true, &reachedEnd)) {
                setNodeOtaJobError("Failed to send node firmware chunk to the OTA coprocessor.");
                return;
            }
            if (nodeOtaJob.uploadOffset == 0 && !reachedEnd) {
                Serial.printf("[NODE OTA]  Sending first coprocessor chunk (%u bytes)\n",
                              (unsigned)COPROC_UPLOAD_CHUNK_DATA_LEN);
            }
            if (reachedEnd) {
                nodeOtaJob.stage = NODE_OTA_JOB_COPROC_FINALIZE;
            }
            break;
        }

        case NODE_OTA_JOB_COPROC_FINALIZE:
            if (!sendCoprocUploadEndFrame(now)) {
                setNodeOtaJobError("Failed to finalize the OTA helper upload.");
                return;
            }
            setNodeOtaJobMessage("Starting OTA helper access point...", 62);
            break;

        case NODE_OTA_JOB_SEND_COMMAND:
            nodeOtaJob.commandAttempts = 1;
            nodeOtaJob.lastCommandAt = now;
            if (!sendNodeOtaBegin(nodeOtaJob.nodeId, nodeOtaJob.commandAttempts)) {
                setNodeOtaJobError("Failed to notify the selected node about OTA mode.");
                return;
            }
            nodeOtaJob.stage = NODE_OTA_JOB_WAIT_TRANSFER;
            setNodeOtaJobMessage("Waiting for node to acknowledge OTA mode...", 68);
            break;

        case NODE_OTA_JOB_WAIT_TRANSFER:
            if (!nodeOtaJob.nodeAccepted && (now - nodeOtaJob.lastCommandAt) >= NODE_OTA_BEGIN_RETRY_MS) {
                if (nodeOtaJob.commandAttempts >= NODE_OTA_BEGIN_MAX_ATTEMPTS) {
                    setNodeOtaJobError("Node did not acknowledge the OTA request.");
                    return;
                }
                nodeOtaJob.commandAttempts++;
                nodeOtaJob.lastCommandAt = now;
                if (!sendNodeOtaBegin(nodeOtaJob.nodeId, nodeOtaJob.commandAttempts)) {
                    setNodeOtaJobError("Failed to resend the OTA request to the selected node.");
                    return;
                }
                setNodeOtaJobMessage("Retrying OTA request to node...", 69);
            }
            if (nodeOtaJob.nodeAccepted &&
                (now - nodeOtaJob.lastCommandAt) > NODE_OTA_TRANSFER_TIMEOUT_MS) {
                setNodeOtaJobError("Node OTA timed out.");
                return;
            }
            if (nodeOtaJob.helperTransferDone) {
                nodeOtaJob.stage = NODE_OTA_JOB_WAIT_REJOIN;
                setNodeOtaJobMessage("Firmware flashed. Waiting for node to reconnect...", 95);
            }
            break;

        case NODE_OTA_JOB_WAIT_REJOIN:
        case NODE_OTA_JOB_SUCCESS:
        case NODE_OTA_JOB_ERROR:
        case NODE_OTA_JOB_IDLE:
        case NODE_OTA_JOB_COPROC_WAIT:
            break;
    }
}

static void handleNodeOtaUpload(AsyncWebServerRequest* req,
                                const String& filename,
                                size_t index,
                                uint8_t* data,
                                size_t len,
                                bool final) {
    if (!isHttpAuthenticated(req)) return;

    auto* state = reinterpret_cast<NodeOtaUploadRequestState*>(req->_tempObject);
    if (index == 0) {
        if (state) {
            if (state->file) state->file.close();
            delete state;
            state = nullptr;
        }
        state = new NodeOtaUploadRequestState();
        req->_tempObject = state;
        state->started = true;
        const AsyncWebHeader* sizeHdr = req->getHeader("X-Firmware-Size");
        if (sizeHdr) state->expectedSize = (size_t)strtoull(sizeHdr->value().c_str(), nullptr, 10);
        strncpy(state->filename, filename.c_str(), sizeof(state->filename) - 1);
        state->filename[sizeof(state->filename) - 1] = '\0';

        if (otaUploadBusy || nodeOtaJob.uploadBusy || nodeOtaJob.active) {
            setNodeOtaUploadError(state, "Another OTA job is already in progress.");
        } else if (!LittleFS.begin(true)) {
            setNodeOtaUploadError(state, "LittleFS is not available.");
        } else {
            if (LittleFS.exists(NODE_OTA_FILE_PATH)) LittleFS.remove(NODE_OTA_FILE_PATH);
            state->file = LittleFS.open(NODE_OTA_FILE_PATH, "w");
            if (!state->file) {
                setNodeOtaUploadError(state, "Could not open temporary storage for node firmware.");
            } else {
                nodeOtaJob.uploadBusy = true;
                Serial.printf("[NODE OTA]  Upload started: \"%s\" (%u bytes)\n",
                              state->filename[0] ? state->filename : "(unnamed)",
                              (unsigned)state->expectedSize);
            }
        }
    }

    if (!state) return;
    if (state->error[0] != '\0') {
        if (final) {
            if (state->file) state->file.close();
            if (LittleFS.exists(NODE_OTA_FILE_PATH)) LittleFS.remove(NODE_OTA_FILE_PATH);
        }
        return;
    }

    if (len > 0) {
        if (index == 0 && data[0] != ESP_IMAGE_HEADER_MAGIC) {
            setNodeOtaUploadError(state, "Uploaded file is not a valid ESP32 application image.");
        }

        scanNodeFirmwareMarkers(state, data, len);
        state->crc32 = crc32Update(state->crc32, data, len);

        const size_t copyLen = std::min<size_t>(len, OTA_DESC_BUF_LEN - state->descBytes);
        if (copyLen > 0) {
            memcpy(state->descBuf + state->descBytes, data, copyLen);
            state->descBytes += copyLen;
            if (state->descBytes >= OTA_DESC_BUF_LEN) {
                validateNodeFirmwareDescriptor(state);
            }
        }

        const size_t written = state->file.write(data, len);
        if (written != len) {
            setNodeOtaUploadError(state, "Failed to store the node firmware image.");
        } else {
            state->written += written;
        }
    }

    if (final && state->error[0] == '\0') {
        state->file.close();
        if (state->written == 0) {
            setNodeOtaUploadError(state, "Empty firmware upload received.");
        } else if (state->expectedSize > 0 && state->written != state->expectedSize) {
            setNodeOtaUploadError(state, String("Upload size mismatch: wrote ")
                + state->written + " of " + state->expectedSize + " bytes.");
        } else if (state->incomingDisplayVersion[0] == '\0' && state->incomingVersion[0] != '\0') {
            strncpy(state->incomingDisplayVersion, state->incomingVersion, sizeof(state->incomingDisplayVersion) - 1);
            state->incomingDisplayVersion[sizeof(state->incomingDisplayVersion) - 1] = '\0';
        }

        if (state->error[0] == '\0' && state->incomingDisplayVersion[0] == '\0') {
            setNodeOtaUploadError(state, "Firmware version marker is missing. Rebuild the node firmware with the current OTA markers.");
        } else if (roleToNodeType(state->incomingRole) == 0) {
            setNodeOtaUploadError(state, "Firmware node-role marker is missing or invalid.");
        } else if (state->incomingHwConfigId[0] == '\0') {
            setNodeOtaUploadError(state, "Firmware hardware configuration marker is missing. Rebuild the node firmware with the current OTA markers.");
        } else if (!parseHardwareConfigId(state->incomingHwConfigId)) {
            setNodeOtaUploadError(state, String("Firmware hardware configuration ID is invalid: ")
                + state->incomingHwConfigId + ".");
        } else {
            state->ok = true;
            Serial.printf("[NODE OTA]  Firmware staged successfully. Role=%s HW=%s Version=%s CRC32=%08X\n",
                          state->incomingRole, state->incomingHwConfigId,
                          state->incomingDisplayVersion, state->crc32);
        }
    }

    if (final && !state->ok) {
        if (state->file) state->file.close();
        if (LittleFS.exists(NODE_OTA_FILE_PATH)) LittleFS.remove(NODE_OTA_FILE_PATH);
        nodeOtaJob.uploadBusy = false;
    }
}

// *****************************************************************************
//  NVS — Gateway config (AP name / password)
// *****************************************************************************
static void loadApConfig() {
    Preferences prefs;
    prefs.begin("gwconfig", true);  // read-only
    String ssid = prefs.getString("ap_ssid", AP_SSID_DEFAULT);
    String pass = prefs.getString("ap_pass", AP_PASS_DEFAULT);
    prefs.end();
    strncpy(gwApSsid,     ssid.c_str(), 32); gwApSsid[32]     = '\0';
    strncpy(gwApPassword, pass.c_str(), 63); gwApPassword[63] = '\0';
    Serial.printf("[CFG]  AP SSID loaded: \"%s\"\n", gwApSsid);
}

static void saveApConfig(const char* ssid, const char* pass) {
    Preferences prefs;
    prefs.begin("gwconfig", false);  // read-write
    prefs.putString("ap_ssid", ssid);
    prefs.putString("ap_pass", pass);
    prefs.end();
    strncpy(gwApSsid,     ssid, 32); gwApSsid[32]     = '\0';
    strncpy(gwApPassword, pass, 63); gwApPassword[63] = '\0';
    Serial.printf("[CFG]  AP config saved: SSID=\"%s\"\n", gwApSsid);
}

// *****************************************************************************
//  NVS — Paired node registry
//  Only the static identity fields are persisted; volatile runtime fields
//  (lastSeen, online, temperature, pressure, uptime, actuators [relays], settings)
//  are repopulated naturally when the node sends its first message.
// *****************************************************************************
// Forward declarations needed by loadNodesFromNvs (defined further below)
static bool addPeer(const uint8_t* mac, uint8_t channel = 0);
static void sendSettingsGet(const uint8_t* mac, uint8_t nodeId, NodeType nodeType);
static void sendSensorSchemaGet(const uint8_t* mac, uint8_t nodeId, NodeType nodeType);
static void sendActuatorSchemaGet(const uint8_t* mac, uint8_t nodeId, NodeType nodeType);
static void sendRfidConfigGet(const uint8_t* mac, uint8_t nodeId, NodeType nodeType);
static void requestNodeDynamicData(uint8_t nodeId);
struct NodeNvsRecordV1 {
    uint8_t  mac[6];
    NodeType type;
    char     name[16];
    char     fw_version[8];
};  // legacy format

struct NodeNvsRecordV2 {
    uint8_t  mac[6];
    NodeType type;
    char     name[16];
    char     fw_version[8];
    char     hw_config_id[HW_CONFIG_ID_LEN];
};  // legacy format with HW config ID

struct NodeNvsRecordV3 {
    uint8_t  mac[6];
    NodeType type;
    char     name[16];
    char     fw_version[8];
    char     hw_config_id[HW_CONFIG_ID_LEN];
    uint32_t capabilities;
    uint8_t  actuator_mask;
    uint8_t  reserved[3];
};  // legacy format with capability flags

struct NodeNvsRecordV4 {
    uint8_t  mac[6];
    NodeType type;
    char     name[MESH_NODE_NAME_LEN];
    char     fw_version[8];
    char     hw_config_id[HW_CONFIG_ID_LEN];
    uint32_t capabilities;
    uint8_t  actuator_mask;
    uint8_t  reserved[3];
};  // current format with expanded node names

static void saveNodesToNvs() {
    Preferences prefs;
    prefs.begin("gwnodes", false);
    prefs.putUChar("nextid", nextId);
    for (uint8_t i = 1; i < nextId; i++) {
        char key[5];
        snprintf(key, sizeof(key), "n%d", i);
        if (nodes[i].mac[0] == 0 && nodes[i].mac[1] == 0) {
            prefs.remove(key);  // slot was freed - remove stale entry
            continue;
        }
        NodeNvsRecordV4 rec = {};
        memcpy(rec.mac,        nodes[i].mac,        6);
        rec.type = nodes[i].type;
        memcpy(rec.name,       nodes[i].name,       sizeof(rec.name));
        memcpy(rec.fw_version, nodes[i].fw_version, 8);
        memcpy(rec.hw_config_id, nodes[i].hw_config_id, sizeof(rec.hw_config_id));
        rec.capabilities = nodes[i].capabilities;
        rec.actuator_mask = nodes[i].actuatorMask;
        prefs.putBytes(key, &rec, sizeof(rec));
    }
    prefs.end();
    nodeRegistryDirty = false;
    nodeRegistryDirtyAtMs = 0;
    // For Debugging: print the saved nodes to the Serial console
    Serial.printf("[MESH] Saved %d nodes to NVS. nextId=%d\n", nextId - 1, nextId);
}

static void markNodeRegistryDirty() {
    nodeRegistryDirty = true;
    nodeRegistryDirtyAtMs = millis();
}

// Called once from setup(), after ESP-NOW is initialised so addPeer() works.
static void loadNodesFromNvs() {
    Preferences prefs;
    prefs.begin("gwnodes", true);
    uint8_t savedNextId = prefs.getUChar("nextid", 1);
    if (savedNextId <= 1) { prefs.end(); return; }

    const uint8_t runtimeNextIdLimit = MESH_MAX_NODES + 1;
    if (savedNextId > runtimeNextIdLimit) {
        Serial.printf("[MESH] Saved node registry nextId=%u exceeds configured MESH_MAX_NODES=%u. Restoring only slots 1..%u.\n",
                      savedNextId, MESH_MAX_NODES, MESH_MAX_NODES);
    }

    uint8_t restored = 0;
    for (uint8_t i = 1; i < savedNextId && i < runtimeNextIdLimit; i++) {
        char key[5];
        snprintf(key, sizeof(key), "n%d", i);
        const size_t blobLen = prefs.getBytesLength(key);
        if (blobLen != sizeof(NodeNvsRecordV1) &&
            blobLen != sizeof(NodeNvsRecordV2) &&
            blobLen != sizeof(NodeNvsRecordV3) &&
            blobLen != sizeof(NodeNvsRecordV4)) continue;

        NodeNvsRecordV4 rec = {};
        if (blobLen == sizeof(NodeNvsRecordV4)) {
            if (prefs.getBytes(key, &rec, sizeof(rec)) != sizeof(rec)) continue;
        } else if (blobLen == sizeof(NodeNvsRecordV3)) {
            NodeNvsRecordV3 legacyRec = {};
            if (prefs.getBytes(key, &legacyRec, sizeof(legacyRec)) != sizeof(legacyRec)) continue;
            memcpy(rec.mac, legacyRec.mac, sizeof(rec.mac));
            rec.type = legacyRec.type;
            memcpy(rec.name, legacyRec.name, sizeof(legacyRec.name));
            memcpy(rec.fw_version, legacyRec.fw_version, sizeof(rec.fw_version));
            memcpy(rec.hw_config_id, legacyRec.hw_config_id, sizeof(rec.hw_config_id));
            rec.capabilities = legacyRec.capabilities;
            rec.actuator_mask = legacyRec.actuator_mask;
        } else if (blobLen == sizeof(NodeNvsRecordV2)) {
            NodeNvsRecordV2 legacyRec = {};
            if (prefs.getBytes(key, &legacyRec, sizeof(legacyRec)) != sizeof(legacyRec)) continue;
            memcpy(rec.mac, legacyRec.mac, sizeof(rec.mac));
            rec.type = legacyRec.type;
            memcpy(rec.name, legacyRec.name, sizeof(legacyRec.name));
            memcpy(rec.fw_version, legacyRec.fw_version, sizeof(rec.fw_version));
            memcpy(rec.hw_config_id, legacyRec.hw_config_id, sizeof(rec.hw_config_id));
            rec.capabilities = defaultCapabilitiesForType(rec.type);
        } else {
            NodeNvsRecordV1 legacyRec = {};
            if (prefs.getBytes(key, &legacyRec, sizeof(legacyRec)) != sizeof(legacyRec)) continue;
            memcpy(rec.mac, legacyRec.mac, sizeof(rec.mac));
            rec.type = legacyRec.type;
            memcpy(rec.name, legacyRec.name, sizeof(legacyRec.name));
            memcpy(rec.fw_version, legacyRec.fw_version, sizeof(rec.fw_version));
            rec.capabilities = defaultCapabilitiesForType(rec.type);
        }
        if (rec.mac[0] == 0 && rec.mac[1] == 0 && rec.mac[2] == 0) continue;

        memcpy(nodes[i].mac,        rec.mac,        6);
        nodes[i].type = rec.type;
        memcpy(nodes[i].name,       rec.name,       sizeof(nodes[i].name));
        memcpy(nodes[i].fw_version, rec.fw_version, 8);
        memcpy(nodes[i].hw_config_id, rec.hw_config_id, sizeof(nodes[i].hw_config_id));
        nodes[i].capabilities = rec.capabilities ? rec.capabilities : defaultCapabilitiesForType(rec.type);
        nodes[i].lastSeen      = millis();  // avoid instant NODE_TIMEOUT
        nodes[i].online        = false;     // marked offline until first message
        nodes[i].actuatorMask = rec.actuator_mask;
        nodes[i].actuatorCount = 0;
        nodes[i].settingsCount = 0;         // re-fetched when node responds

        addPeer(rec.mac);  // re-register ESP-NOW peer
        restored++;
        if (nodes[i].hw_config_id[0] != '\0') {
            Serial.printf("[MESH] Restored node #%d \"%s\"  %s  hw=%s\n",
                          i, nodes[i].name, macToStr(nodes[i].mac).c_str(),
                          nodes[i].hw_config_id);
        } else {
            Serial.printf("[MESH] Restored node #%d \"%s\"  %s\n",
                          i, nodes[i].name, macToStr(nodes[i].mac).c_str());
        }
    }
    nextId = savedNextId;
    if (nextId > runtimeNextIdLimit) nextId = runtimeNextIdLimit;
    prefs.end();

    if (restored == 0) return;
    Serial.printf("[MESH] %d node(s) restored from NVS. nextId=%d\n", restored, nextId);

    for (uint8_t i = 1; i < nextId; i++) {
        if (nodes[i].mac[0] == 0 && nodes[i].mac[1] == 0) continue;
        loadRelayLabelsForNode(i);
    }

    // Proactively request both the settings and sensor schema from every restored
    // node so the dashboard and settings panel repopulate as soon as each node replies.
    for (uint8_t i = 1; i < nextId; i++) {
        if (nodes[i].mac[0] == 0 && nodes[i].mac[1] == 0) continue;
        requestNodeDynamicData(i);
    }
}

// *****************************************************************************
//  LittleFS Helpers
// *****************************************************************************
static void mountFilesystem() {
    if (!LittleFS.begin(false, "/littlefs", 10, "spiffs")) {
        Serial.println("[FS]  CRITICAL: LittleFS mount failed!");
        Serial.println("[FS]  Run 'pio run --target uploadfs' then reboot.");
        while (true) { delay(1000); }
    }
    Serial.printf("[FS]  Mounted. Total: %u KB  Used: %u KB\n",
                  LittleFS.totalBytes() / 1024, LittleFS.usedBytes() / 1024);
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
        Serial.printf("[FS]    %-36s  %6u B\n", file.path(), file.size());
        file = root.openNextFile();
    }
}

// *****************************************************************************
//  ESP-NOW helpers
// *****************************************************************************
static bool addPeer(const uint8_t* mac, uint8_t channel) {
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t p{};
    memcpy(p.peer_addr, mac, 6);
    p.channel = channel;
    p.encrypt = false;

    // For Debugging: print the peer being added to the Serial console
    Serial.printf("[MESH] Adding ESP-NOW peer: %s on channel %d... ", macToStr(mac).c_str(), channel);
    return esp_now_add_peer(&p) == ESP_OK;
}

static void sendRegisterAck(const uint8_t* mac, uint8_t assignedId, NodeType nodeType) {
    MsgRegisterAck ack;
    ack.hdr.type      = MSG_REGISTER_ACK;
    ack.hdr.node_id   = assignedId;
    ack.hdr.node_type = nodeType;
    ack.assigned_id   = assignedId;
    ack.channel       = wifiChannel;

    // For Debugging: print the ACK being sent to the Serial console
    Serial.printf("[MESH] Sending registration ACK to %s: assigned ID #%d on channel %d\n",
                  macToStr(mac).c_str(), assignedId, wifiChannel);
    esp_now_send(mac, (uint8_t*)&ack, sizeof(ack));
}

static void sendPairCmd(const uint8_t* mac) {
    MsgPairCmd cmd;
    cmd.hdr.type      = MSG_PAIR_CMD;
    cmd.hdr.node_id   = 0;
    cmd.hdr.node_type = NODE_SENSOR;
    cmd.channel       = wifiChannel;

    // For Debugging: print the command being sent to the Serial console
    Serial.printf("[MESH] Sending pair command to %s on channel %d\n",
                  macToStr(mac).c_str(), wifiChannel);
    esp_now_send(mac, (uint8_t*)&cmd, sizeof(cmd));
}

static void sendUnpairCmd(const uint8_t* mac, uint8_t nodeId) {
    MsgUnpairCmd cmd;
    cmd.hdr.type      = MSG_UNPAIR_CMD;
    cmd.hdr.node_id   = nodeId;
    cmd.hdr.node_type = NODE_SENSOR;

    // For Debugging: print the command being sent to the Serial console
    Serial.printf("[MESH] Sending UNPAIR command to node #%d (%s)\n",
                  nodeId, macToStr(mac).c_str());
    esp_now_send(mac, (uint8_t*)&cmd, sizeof(cmd));
}

static void sendRebootCmd(const uint8_t* mac, uint8_t nodeId) {
    MsgRebootCmd cmd;
    cmd.hdr.type      = MSG_REBOOT_CMD;
    cmd.hdr.node_id   = nodeId;
    cmd.hdr.node_type = NODE_SENSOR;  // node ignores this field on reboot

    // For Debugging: print the command being sent to the Serial console
    Serial.printf("[MESH] Sending reboot command to node #%d (%s)\n",
                  nodeId, macToStr(mac).c_str());
    esp_now_send(mac, (uint8_t*)&cmd, sizeof(cmd));
}

static void sendSettingsGet(const uint8_t* mac, uint8_t nodeId, NodeType nodeType) {
    MsgSettingsGet msg;
    msg.hdr.type      = MSG_SETTINGS_GET;
    msg.hdr.node_id   = nodeId;
    msg.hdr.node_type = nodeType;

    // For Debugging: print the request being sent to the Serial console
    Serial.printf("[MESH] Requesting settings from node #%d (%s)\n",
                  nodeId, macToStr(mac).c_str());
    esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

static void sendSensorSchemaGet(const uint8_t* mac, uint8_t nodeId, NodeType nodeType) {
    MsgSensorSchemaGet msg;
    msg.hdr.type      = MSG_SENSOR_SCHEMA_GET;
    msg.hdr.node_id   = nodeId;
    msg.hdr.node_type = nodeType;

    // For Debugging: print the request being sent to the Serial console
    Serial.printf("[MESH] Requesting sensor schema from node #%d (%s)\n",
                  nodeId, macToStr(mac).c_str());
    esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

static void sendActuatorSchemaGet(const uint8_t* mac, uint8_t nodeId, NodeType nodeType) {
    MsgActuatorSchemaGet msg;
    msg.hdr.type      = MSG_ACTUATOR_SCHEMA_GET;
    msg.hdr.node_id   = nodeId;
    msg.hdr.node_type = nodeType;

    Serial.printf("[MESH] Requesting actuator schema from node #%d (%s)\n",
                  nodeId, macToStr(mac).c_str());
    esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

static void sendRfidConfigGet(const uint8_t* mac, uint8_t nodeId, NodeType nodeType) {
    MsgRfidConfigGet msg;
    msg.hdr.type      = MSG_RFID_CONFIG_GET;
    msg.hdr.node_id   = nodeId;
    msg.hdr.node_type = nodeType;

    Serial.printf("[MESH] Requesting RFID config from node #%d (%s)\n",
                  nodeId, macToStr(mac).c_str());
    esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

static void requestNodeDynamicData(uint8_t nodeId) {
    if (nodeId == 0 || nodeId >= nextId) return;
    if (nodes[nodeId].mac[0] == 0) return;

    sendSettingsGet(nodes[nodeId].mac, nodeId, nodes[nodeId].type);
    if (nodes[nodeId].capabilities & NODE_CAP_SENSOR_DATA) {
        sendSensorSchemaGet(nodes[nodeId].mac, nodeId, nodes[nodeId].type);
    }
    if (nodes[nodeId].capabilities & NODE_CAP_ACTUATORS) {
        sendActuatorSchemaGet(nodes[nodeId].mac, nodeId, nodes[nodeId].type);
    }
    if (nodes[nodeId].capabilities & NODE_CAP_RFID) {
        sendRfidConfigGet(nodes[nodeId].mac, nodeId, nodes[nodeId].type);
    }
}

static void sendSettingsSet(const uint8_t* mac, uint8_t nodeId, NodeType nodeType,
                            uint8_t settingId, int16_t value) {
    
    if (nodeId == 0 || nodeId >= nextId) return;
    
    MsgSettingsSet msg;
    msg.hdr.type      = MSG_SETTINGS_SET;
    msg.hdr.node_id   = nodeId;
    msg.hdr.node_type = nodeType;
    msg.id            = settingId;
    msg.value         = value;
    
    // For Debugging: print the settings update being sent to the Serial console
    Serial.printf("[MESH] Sending settings update to node #%d: setting_id=%d value=%d\n",
                  nodeId, settingId, value);
    esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

static bool sendActuatorCmd(uint8_t nodeId, uint8_t actuatorId, uint8_t state)
{
    if (nodeId == 0 || nodeId >= nextId) return false;
    if (!(nodes[nodeId].capabilities & NODE_CAP_ACTUATORS)) return false;

    MsgActuatorSet cmd;

    cmd.hdr.type      = MSG_ACTUATOR_SET;
    cmd.hdr.node_id   = nodeId;
    cmd.hdr.node_type = nodes[nodeId].type;

    cmd.actuator_id = actuatorId;
    cmd.state       = state;

    // For Debugging: print the command being sent to the Serial console
    Serial.printf("[MESH] Sending actuator cmd to node #%d: actuator_id=%d state=%d\n",
                  nodeId, actuatorId, state);

    esp_err_t r = esp_now_send(nodes[nodeId].mac, (uint8_t*)&cmd, sizeof(cmd));
    if (r != ESP_OK) {
        Serial.printf("[MESH] Failed to send actuator cmd to node #%d: error\n", nodeId);
        return false;
    }
    Serial.printf("[MESH] Actuator cmd sent to node #%d successfully\n", nodeId);
    return true;
}

// *****************************************************************************
//  ESP-NOW callbacks
// *****************************************************************************
static void onDataRecv(const esp_now_recv_info_t* info,
                       const uint8_t* data, int len) {
    RxPacket pkt;
    memcpy(pkt.mac, info->src_addr, 6);
    int l = (len < (int)sizeof(pkt.data)) ? len : (int)sizeof(pkt.data);
    memcpy(pkt.data, data, l);
    pkt.len = l;
    xQueueSend(rxQueue, &pkt, 0);
}

static void onDataSent(const wifi_tx_info_t*, esp_now_send_status_t) {}

// *****************************************************************************
//  Discovered list management
// *****************************************************************************
// Returns true if the discovered list had a visible change (new node appeared).
static bool updateDiscovered(const uint8_t* mac, const char* name,
                              NodeType type, uint8_t txChannel) {
    unsigned long now = millis();

    for (uint8_t i = 0; i < MAX_DISCOVERED; i++) {
        if (discovered[i].active && memcmp(discovered[i].mac, mac, 6) == 0) {
            discovered[i].lastSeen  = now;
            discovered[i].tx_channel = txChannel;
            return false;  // refresh only, no UI change
        }
    }

    // New entry - find a free or expired slot
    for (uint8_t i = 0; i < MAX_DISCOVERED; i++) {
        if (!discovered[i].active ||
            (now - discovered[i].lastSeen > DISCOVERED_TIMEOUT_MS)) {
            memcpy(discovered[i].mac, mac, 6);
            strncpy(discovered[i].name, name, sizeof(discovered[i].name) - 1);
            discovered[i].name[sizeof(discovered[i].name) - 1] = '\0';
            discovered[i].type      = type;
            discovered[i].tx_channel = txChannel;
            discovered[i].lastSeen  = now;
            discovered[i].active    = true;
            Serial.printf("[DISC]  New: \"%s\" %s ch%d\n",
                          name, macToStr(mac).c_str(), txChannel);
            return true;  // UI update needed
        }
    }
    return false;
}

// Returns true if any entry expired (UI update needed)
static bool cleanupDiscovered() {
    unsigned long now = millis();
    bool changed = false;
    for (uint8_t i = 0; i < MAX_DISCOVERED; i++) {
        if (discovered[i].active &&
            (now - discovered[i].lastSeen > DISCOVERED_TIMEOUT_MS)) {
            discovered[i].active = false;
            Serial.printf("[DISC]  Expired: %s\n", macToStr(discovered[i].mac).c_str());
            changed = true;
        }
    }
    return changed;
}

// *****************************************************************************
//  Disconnect a paired node
// *****************************************************************************
static String buildNodesJson();  // forward declaration
static void disconnectNode(uint8_t nodeId) {
    if (nodeId == 0 || nodeId >= nextId) return;

    uint8_t mac[6];
    memcpy(mac, nodes[nodeId].mac, 6);

    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(&nodes[nodeId], 0, sizeof(NodeRecord));  // free the slot
        xSemaphoreGive(nodesMutex);
    }

    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
    if(gwLedEnabled) {
        flashGwLed(gwLed.Color(255, 80, 0), 200);  // orange pulse
    }
    else {    
        setGwLed(0); // turn off immediately
    }
    Serial.printf("[MESH] Node #%d disconnected\n", nodeId);
    removeRelayLabelsForMac(mac);
    saveNodesToNvs();  // persist the freed slot so it doesn't reappear after reboot
    wsBroadcast(buildNodesJson());  // forward declaration - defined below
}

// *****************************************************************************
//  JSON builders
// *****************************************************************************
static String buildNodesJson() {
    JsonDocument doc;
    doc["type"] = "update";
    JsonArray arr = doc["nodes"].to<JsonArray>();

    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        unsigned long now = millis();
        for (uint8_t i = 1; i < nextId; i++) {
            if (nodes[i].mac[0] == 0 && nodes[i].mac[1] == 0) continue;
            if (now - nodes[i].lastSeen > NODE_TIMEOUT_MS)
                nodes[i].online = false;

            JsonObject n = arr.add<JsonObject>();
            n["id"]         = i;
            n["name"]       = nodes[i].name;
            n["mac"]        = macToStr(nodes[i].mac);
            n["type"]       = (int)nodes[i].type;
            n["online"]     = nodes[i].online;
            n["last_seen"]  = (int)((now - nodes[i].lastSeen) / 1000);
            n["uptime"]     = nodes[i].uptime;
            n["fw_version"] = nodes[i].fw_version;
            n["hw_config_id"] = nodes[i].hw_config_id;
            n["capabilities"] = nodes[i].capabilities;
            n["has_sensor_data"] = nodeSupportsCapability(nodes[i], NODE_CAP_SENSOR_DATA);
            n["has_actuators"] = nodeSupportsCapability(nodes[i], NODE_CAP_ACTUATORS);
            n["has_rfid"] = nodeSupportsCapability(nodes[i], NODE_CAP_RFID);
            n["settings_ready"] = (nodes[i].settingsCount > 0);
            n["sensor_schema_ready"] = (nodes[i].sensorCount > 0);
            n["actuator_schema_ready"] = (nodes[i].actuatorCount > 0);
            n["actuator_count"] = defaultActuatorCountForNode(nodes[i]);
            n["rfid_ready"] = nodes[i].rfidConfigReady;

            if (nodeSupportsCapability(nodes[i], NODE_CAP_SENSOR_DATA)) {
                // sensor_schema_ready mirrors sensorCount > 0, used by the JS client
                // to decide whether to request the schema via node_sensor_schema_get.
                n["sensor_schema_ready"] = (nodes[i].sensorCount > 0);
                JsonArray rdgs = n["sensor_readings"].to<JsonArray>();
                for (uint8_t j = 0; j < nodes[i].sensorCount; j++) {
                    JsonObject r = rdgs.add<JsonObject>();
                    r["id"]    = nodes[i].sensorSchema[j].id;
                    r["value"] = nodes[i].sensorValues[j];
                }
            }

            if (nodeSupportsCapability(nodes[i], NODE_CAP_ACTUATORS)) {
                n["actuator_mask"] = nodes[i].actuatorMask;
                n["relay_mask"]    = nodes[i].actuatorMask;  // backward-compatible alias for older UI code
                JsonArray labels = n["relay_labels"].to<JsonArray>();
                const uint8_t relayCount = defaultActuatorCountForNode(nodes[i]);
                for (uint8_t j = 0; j < relayCount; j++) {
                    labels.add(nodes[i].relayLabels[j][0]
                        ? nodes[i].relayLabels[j]
                        : String("Relay ") + String(j + 1));
                }
            }
        }
        xSemaphoreGive(nodesMutex);
    }

    String out;
    serializeJson(doc, out);
    return out;
}

static String buildMetaJson() {
    JsonDocument doc;
    doc["type"]            = "meta";
    doc["ip"]              = WiFi.localIP().toString();
    doc["mac"]             = WiFi.macAddress();
    doc["channel"]         = wifiChannel;
    doc["uptime"]          = (millis() - bootMs) / 1000;
    doc["fw_version"]      = FW_VERSION;
    doc["hw_config_id"]    = HW_CONFIG_ID;
    doc["gw_led_enabled"] = gwLedEnabled;
    doc["ap_ssid"]         = gwApSsid;
    doc["credentials_set"] = credentialsSet();
    doc["ota_supported"]   = gatewayOtaSupported();
    doc["ota_busy"]        = otaUploadBusy;
    doc["ota_max_bytes"]   = (uint32_t)getGatewayOtaSlotSize();
    doc["ota_project"]     = esp_app_get_description()->project_name;
    doc["node_ota_busy"]   = nodeOtaJob.active || nodeOtaJob.uploadBusy;
    doc["node_ota_helper_online"] = coprocOnline;
    doc["node_ota_host"]   = NODE_OTA_HOST;
    String out;
    serializeJson(doc, out);
    return out;
}

static String buildRelayLabelsAckJson(uint8_t nodeId, bool ok, const char* err = nullptr) {
    JsonDocument doc;
    doc["type"] = "relay_labels_ack";
    doc["ok"] = ok;
    doc["node_id"] = nodeId;
    if (!ok && err) doc["err"] = err;

    if (ok && nodeId > 0 && nodeId < nextId) {
        JsonArray labels = doc["labels"].to<JsonArray>();
        if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (uint8_t i = 0; i < NODE_MAX_ACTUATORS; i++) {
                labels.add(nodes[nodeId].relayLabels[i]);
            }
            xSemaphoreGive(nodesMutex);
        }
    }

    String out;
    serializeJson(doc, out);
    return out;
}

// Builds the node_settings WS message for a specific node.
// type:"node_settings" is pushed only to clients that requested it.
static String buildNodeSettingsJson(uint8_t nodeId) {
    JsonDocument doc;
    doc["type"]    = "node_settings";
    doc["node_id"] = nodeId;
    JsonArray arr  = doc["settings"].to<JsonArray>();

    if (nodeId == 0 || nodeId >= nextId) {
        String out; serializeJson(doc, out); return out;
    }

    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const NodeRecord& n = nodes[nodeId];
        for (uint8_t i = 0; i < n.settingsCount; i++) {
            const SettingDef& s = n.settings[i];
            JsonObject obj      = arr.add<JsonObject>();
            obj["id"]           = s.id;
            obj["type"]         = (int)s.type;
            obj["label"]        = s.label;
            obj["current"]      = s.current;
            if (s.type == SETTING_INT) {
                obj["i_min"]  = s.i_min;
                obj["i_max"]  = s.i_max;
                obj["i_step"] = s.i_step;
            }
            if (s.type == SETTING_ENUM) {
                obj["opt_count"] = s.opt_count;
                JsonArray opts   = obj["opts"].to<JsonArray>();
                for (uint8_t j = 0; j < s.opt_count && j < SETTING_OPT_MAXCOUNT; j++)
                    opts.add(s.opts[j]);
            }
        }
        xSemaphoreGive(nodesMutex);
    }

    String out;
    serializeJson(doc, out);
    return out;
}

// Builds the node_sensor_schema WS message for a specific sensor node.
// Pushed to clients on schema receipt; also served on demand via
// the "node_sensor_schema_get" WS command.
static String buildNodeSensorSchemaJson(uint8_t nodeId) {
    JsonDocument doc;
    doc["type"]    = "node_sensor_schema";
    doc["node_id"] = nodeId;
    JsonArray arr  = doc["sensors"].to<JsonArray>();

    if (nodeId == 0 || nodeId >= nextId) {
        String out; serializeJson(doc, out); return out;
    }

    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const NodeRecord& n = nodes[nodeId];
        for (uint8_t i = 0; i < n.sensorCount; i++) {
            const SensorDef& s = n.sensorSchema[i];
            JsonObject obj  = arr.add<JsonObject>();
            obj["id"]        = s.id;
            obj["label"]     = s.label;
            obj["unit"]      = s.unit;
            obj["precision"] = s.precision;
        }
        xSemaphoreGive(nodesMutex);
    }

    String out;
    serializeJson(doc, out);
    return out;
}

static String bytesToHexString(const uint8_t* data, uint8_t len) {
    static const char* hex = "0123456789ABCDEF";
    String out;
    out.reserve(len * 2);
    for (uint8_t i = 0; i < len; i++) {
        out += hex[(data[i] >> 4) & 0x0F];
        out += hex[data[i] & 0x0F];
    }
    return out;
}

static int hexNibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
    if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
    return -1;
}

static bool parseRfidUidHex(const char* text, uint8_t* outUid, uint8_t* outLen) {
    if (!text || !outUid || !outLen) return false;
    uint8_t bytes[RFID_UID_MAX_LEN] = {0};
    uint8_t nibbleCount = 0;
    int hi = -1;
    while (*text != '\0') {
        const char ch = *text++;
        if (ch == ' ' || ch == ':' || ch == '-' || ch == '.') continue;
        int nibble = hexNibble(ch);
        if (nibble < 0) return false;
        if (hi < 0) {
            hi = nibble;
        } else {
            if ((nibbleCount / 2) >= RFID_UID_MAX_LEN) return false;
            bytes[nibbleCount / 2] = (uint8_t)((hi << 4) | nibble);
            nibbleCount += 2;
            hi = -1;
        }
    }
    if (hi >= 0) return false;
    const uint8_t uidLen = nibbleCount / 2;
    if (!(uidLen == 4 || uidLen == 7 || uidLen == 10)) return false;
    memcpy(outUid, bytes, uidLen);
    *outLen = uidLen;
    return true;
}

static String buildNodeActuatorSchemaJson(uint8_t nodeId) {
    JsonDocument doc;
    doc["type"] = "node_actuator_schema";
    doc["node_id"] = nodeId;
    JsonArray arr = doc["actuators"].to<JsonArray>();

    if (nodeId == 0 || nodeId >= nextId) {
        String out; serializeJson(doc, out); return out;
    }

    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const NodeRecord& n = nodes[nodeId];
        doc["count"] = n.actuatorCount;
        for (uint8_t i = 0; i < n.actuatorCount; i++) {
            JsonObject obj = arr.add<JsonObject>();
            obj["id"] = n.actuatorSchema[i].id;
            obj["label"] = n.actuatorSchema[i].label;
        }
        xSemaphoreGive(nodesMutex);
    }

    String out;
    serializeJson(doc, out);
    return out;
}

static String buildNodeRfidConfigJson(uint8_t nodeId) {
    JsonDocument doc;
    doc["type"] = "node_rfid_config";
    doc["node_id"] = nodeId;

    if (nodeId == 0 || nodeId >= nextId) {
        String out; serializeJson(doc, out); return out;
    }

    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const NodeRecord& n = nodes[nodeId];
        doc["ready"] = n.rfidConfigReady;
        doc["actuator_count"] = defaultActuatorCountForNode(n);
        JsonArray slots = doc["slots"].to<JsonArray>();
        for (uint8_t i = 0; i < RFID_MAX_SLOTS; i++) {
            const GatewayRfidSlot& slot = n.rfidSlots[i];
            JsonObject obj = slots.add<JsonObject>();
            obj["slot"] = i;
            obj["enabled"] = slot.enabled;
            obj["uid"] = slot.uidLen ? bytesToHexString(slot.uid, slot.uidLen) : "";
            obj["uid_len"] = slot.uidLen;
            obj["relay_mask"] = slot.relayMask;
        }
        doc["last_uid"] = n.lastRfidUidLen ? bytesToHexString(n.lastRfidUid, n.lastRfidUidLen) : "";
        doc["last_uid_len"] = n.lastRfidUidLen;
        doc["last_matched_slot"] = n.lastRfidMatchedSlot;
        doc["last_relay_mask"] = n.lastRfidAppliedMask;
        doc["last_seen_ms"] = n.lastRfidSeenAt;
        xSemaphoreGive(nodesMutex);
    }

    String out;
    serializeJson(doc, out);
    return out;
}

static String buildRfidConfigAckJson(uint8_t nodeId, bool ok, const char* err = nullptr) {
    JsonDocument doc;
    doc["type"] = "node_rfid_config_ack";
    doc["node_id"] = nodeId;
    doc["ok"] = ok;
    if (!ok && err) doc["err"] = err;
    String out;
    serializeJson(doc, out);
    return out;
}

static String buildRfidScanEventJson(uint8_t nodeId) {
    JsonDocument doc;
    doc["type"] = "rfid_scan_event";
    doc["node_id"] = nodeId;
    if (nodeId > 0 && nodeId < nextId && xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const NodeRecord& n = nodes[nodeId];
        doc["uid"] = n.lastRfidUidLen ? bytesToHexString(n.lastRfidUid, n.lastRfidUidLen) : "";
        doc["uid_len"] = n.lastRfidUidLen;
        doc["matched_slot"] = n.lastRfidMatchedSlot;
        doc["relay_mask"] = n.lastRfidAppliedMask;
        doc["seen_ms"] = n.lastRfidSeenAt;
        xSemaphoreGive(nodesMutex);
    }
    String out;
    serializeJson(doc, out);
    return out;
}

static String buildDiscoveredJson() {
    JsonDocument doc;
    doc["type"] = "discovered";
    JsonArray arr = doc["nodes"].to<JsonArray>();
    unsigned long now = millis();
    for (uint8_t i = 0; i < MAX_DISCOVERED; i++) {
        if (!discovered[i].active) continue;
        if (now - discovered[i].lastSeen > DISCOVERED_TIMEOUT_MS) {
            discovered[i].active = false;
            continue;
        }
        JsonObject n = arr.add<JsonObject>();
        n["mac"]  = macToStr(discovered[i].mac);
        n["name"] = discovered[i].name;
        n["type"] = (int)discovered[i].type;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

// *****************************************************************************
//  Process RX queue — called from loop()
// *****************************************************************************
static void processRxQueue() {
    RxPacket pkt;
    while (xQueueReceive(rxQueue, &pkt, 0) == pdTRUE) {
        if (pkt.len < (int)sizeof(MeshHeader)) continue;
        auto* hdr = (MeshHeader*)pkt.data;

        switch (hdr->type) {

            // Beacon from node in pairing mode
            case MSG_BEACON: {
                if (pkt.len < (int)sizeof(MsgBeacon)) break;
                auto* b = (MsgBeacon*)pkt.data;

                // Ignore if this node is already paired
                if (findNodeByMac(pkt.mac) != 0) break;

                bool isNew = updateDiscovered(pkt.mac, b->name,
                                              b->hdr.node_type, b->tx_channel);
                if (isNew) {
                    if (gwLedEnabled) {
                        flashGwLed(gwLed.Color(255, 255, 255), 150);  // white pulse
                    }
                    else {
                        setGwLed(255); // turn on solid white
                    }
                    wsBroadcast(buildDiscoveredJson());
                }
                break;
            }

            // Registration (paired reboot reconnect OR post-PAIR_CMD confirm)
            case MSG_REGISTER: {
                if (pkt.len < MSG_REGISTER_MIN_LEN) break;
                auto* reg = (MsgRegister*)pkt.data;

                uint8_t assignId = findNodeByMac(pkt.mac);
                bool    isNew    = (assignId == 0);
                if (isNew) {
                    assignId = findFreeSlot();
                    if (assignId == 0) {
                        Serial.println("[MESH] Max nodes reached");
                        if (pendingPair.active && memcmp(pendingPair.mac, pkt.mac, 6) == 0) {
                            char macStr[18];
                            snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                                     pkt.mac[0], pkt.mac[1], pkt.mac[2],
                                     pkt.mac[3], pkt.mac[4], pkt.mac[5]);
                            pendingPair.active = false;
                            wsBroadcast(buildPairCapacityFullJson(macStr));
                        }
                        break;
                    }
                }

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    memcpy(nodes[assignId].mac, pkt.mac, 6);
                    nodes[assignId].type     = hdr->node_type;
                    nodes[assignId].capabilities = defaultCapabilitiesForType(hdr->node_type);
                    strncpy(nodes[assignId].name, reg->name, sizeof(nodes[assignId].name) - 1);
                    nodes[assignId].name[sizeof(nodes[assignId].name) - 1] = '\0';
                    strncpy(nodes[assignId].fw_version, reg->fw_version, 7);
                    nodes[assignId].fw_version[7] = '\0';
                    memset(nodes[assignId].hw_config_id, 0, sizeof(nodes[assignId].hw_config_id));
                    if (pkt.len >= (int)sizeof(MsgRegister)) {
                        strncpy(nodes[assignId].hw_config_id, reg->hw_config_id,
                                sizeof(nodes[assignId].hw_config_id) - 1);
                    }
                    if (pkt.len >= MSG_REGISTER_CAPS_LEN) {
                        nodes[assignId].capabilities = reg->capabilities;
                    }
                    nodes[assignId].lastSeen = millis();
                    nodes[assignId].online   = true;
                    if (isNew) {
                        nodes[assignId].settingsCount = 0;
                        nodes[assignId].sensorCount = 0;
                        nodes[assignId].rfidConfigReady = false;
                        memset(nodes[assignId].rfidSlots, 0, sizeof(nodes[assignId].rfidSlots));
                    }
                    if (isNew) nodes[assignId].actuatorCount = 0;
                    xSemaphoreGive(nodesMutex);
                }

                if (isNew) {
                    loadRelayLabelsForNode(assignId);
                }

                if (nodes[assignId].hw_config_id[0] != '\0') {
                    Serial.printf("[MESH] %s node #%d \"%s\"  %s  hw=%s\n",
                                  isNew ? "New" : "Re-reg",
                                  assignId, reg->name, macToStr(pkt.mac).c_str(),
                                  nodes[assignId].hw_config_id);
                } else {
                    Serial.printf("[MESH] %s node #%d \"%s\"  %s\n",
                                  isNew ? "New" : "Re-reg",
                                  assignId, reg->name, macToStr(pkt.mac).c_str());
                }

                // Cancel pending pair if this is the expected MAC
                if (pendingPair.active &&
                    memcmp(pendingPair.mac, pkt.mac, 6) == 0) {
                    pendingPair.active = false;
                    Serial.println("[PAIR]  Pair confirmed.");
                    if (gwLedEnabled) {
                        flashGwLed(gwLed.Color(0, 255, 0), 300);  // green pulse
                    }
                    else {
                        setGwLed(0); // turn off immediately
                    }
                }

                // Remove from discovered list if it was there
                for (uint8_t i = 0; i < MAX_DISCOVERED; i++) {
                    if (discovered[i].active &&
                        memcmp(discovered[i].mac, pkt.mac, 6) == 0) {
                        discovered[i].active = false;
                    }
                }

                addPeer(pkt.mac);
                sendRegisterAck(pkt.mac, assignId, hdr->node_type);
                // Request dynamic node data according to the capability flags
                // advertised by the node.
                requestNodeDynamicData(assignId);
                // Persist every registration so firmware/version/hardware metadata
                // survives gateway restarts and older saved records get upgraded.
                saveNodesToNvs();

                if (nodeOtaJob.active &&
                    nodeOtaJob.nodeId == assignId) {
                    nodeOtaJob.lastActivityAt = millis();
                    const bool versionMatches = strncmp(nodes[assignId].fw_version,
                                                        nodeOtaJob.version,
                                                        sizeof(nodes[assignId].fw_version) - 1) == 0;
                    const bool versionChangedAcrossOta =
                        strncmp(nodeOtaJob.priorVersion,
                                nodeOtaJob.version,
                                sizeof(nodeOtaJob.priorVersion) - 1) != 0;
                    const bool timeoutRecovery =
                        nodeOtaJob.stage == NODE_OTA_JOB_ERROR &&
                        strncmp(nodeOtaJob.error, "Node OTA timed out.", sizeof(nodeOtaJob.error)) == 0;
                    const bool otaCompletionConfirmed =
                        nodeOtaJob.helperTransferDone || (versionChangedAcrossOta && versionMatches);
                    if ((nodeOtaJob.stage == NODE_OTA_JOB_WAIT_TRANSFER ||
                         nodeOtaJob.stage == NODE_OTA_JOB_WAIT_REJOIN ||
                         timeoutRecovery) &&
                        otaCompletionConfirmed) {
                        nodeOtaJob.stage = NODE_OTA_JOB_SUCCESS;
                        setNodeOtaJobMessage(String("Node reconnected with firmware v") + nodes[assignId].fw_version + ".", 100);
                        broadcastNodeOtaState();
                    } else if (timeoutRecovery) {
                        setNodeOtaJobMessage("Node reconnected. Waiting for final OTA confirmation...", 96);
                    }
                }

                wsBroadcast(buildNodesJson());
                wsBroadcast(buildDiscoveredJson());
                continue;
            }

            case MSG_SENSOR_DATA: {
                // Minimum: header(3) + uptime_sec(4) + count(1) = 8 bytes.
                // A valid packet must also be large enough for the declared count.
                if (pkt.len < 8) break;
                auto* sd   = (MsgSensorData*)pkt.data;
                uint8_t id = hdr->node_id;
                if (id == 0 || id >= nextId) break;
                if (nodes[id].mac[0] == 0) break;  // slot was recycled

                uint8_t cnt = sd->count;
                if (cnt > NODE_MAX_SENSORS) cnt = NODE_MAX_SENSORS;

                // Validate packet is large enough for the declared number of readings
                size_t expectedLen = sizeof(MeshHeader) + sizeof(uint32_t) + 1
                                     + cnt * sizeof(SensorReading);
                if ((size_t)pkt.len < expectedLen) {
                    Serial.printf("[SENS] Node #%d data pkt too short (%d < %d)\n",
                                  id, pkt.len, (int)expectedLen);
                    break;
                }

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    nodes[id].uptime   = sd->uptime_sec;
                    nodes[id].lastSeen = millis();
                    nodes[id].online   = true;

                    // Match each incoming reading to its schema position by sensor ID.
                    // This allows the node to send a subset of sensors in any cycle
                    // without invalidating values from sensors it skipped.
                    for (uint8_t k = 0; k < cnt; k++) {
                        for (uint8_t j = 0; j < nodes[id].sensorCount; j++) {
                            if (nodes[id].sensorSchema[j].id == sd->readings[k].id) {
                                nodes[id].sensorValues[j] = sd->readings[k].value;
                                break;
                            }
                        }
                    }
                    xSemaphoreGive(nodesMutex);
                }
                Serial.printf("[SENS] Node #%d  uptime=%us  readings=%u\n",
                              id, sd->uptime_sec, cnt);
                break;
            }

            case MSG_ACTUATOR_STATE: {
                if (pkt.len < (int)sizeof(MeshHeader) + 1) break;

                auto* rs = (MsgActuatorState*)pkt.data;
                uint8_t id = hdr->node_id;

                if (id == 0 || id >= nextId) break;
                
                uint8_t count = rs->count;
                if (count > NODE_MAX_ACTUATORS) count = NODE_MAX_ACTUATORS;

                size_t expected =
                    sizeof(MeshHeader) +
                    1 +
                    count * sizeof(ActuatorState);

                if (pkt.len < expected) break;

                uint8_t mask = 0;

                // Build bitmask from actuator states
                for (uint8_t i = 0; i < rs->count; i++) {
                    uint8_t actuatorId = rs->states[i].id;
                    uint8_t state      = rs->states[i].state;

                    if (actuatorId < 8 && state) {
                        mask |= (1 << actuatorId);
                    }
                }

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (nodes[id].actuatorMask != mask) {
                        nodes[id].actuatorMask = mask;
                        markNodeRegistryDirty();
                    }
                    nodes[id].lastSeen = millis();
                    nodes[id].online   = true;
                    xSemaphoreGive(nodesMutex);
                }

                wsBroadcast(buildNodesJson());

                Serial.printf("[MESH] Node #%d actuator mask: 0x%02X\n", id, mask);

                break;
            }

            case MSG_ACTUATOR_SCHEMA: {
                if (pkt.len < (int)sizeof(MeshHeader) + 1) break;
                const uint8_t id = hdr->node_id;
                if (id == 0 || id >= nextId) break;
                if (nodes[id].mac[0] == 0) break;

                auto* as = (MsgActuatorSchema*)pkt.data;
                uint8_t count = as->count;
                if (count > NODE_MAX_ACTUATORS) count = NODE_MAX_ACTUATORS;
                const size_t expectedLen = sizeof(MeshHeader) + 1 + count * sizeof(ActuatorDef);
                if ((size_t)pkt.len < expectedLen) break;

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    nodes[id].actuatorCount = count;
                    for (uint8_t i = 0; i < count; i++) {
                        nodes[id].actuatorSchema[i] = as->actuators[i];
                    }
                    nodes[id].lastSeen = millis();
                    nodes[id].online = true;
                    xSemaphoreGive(nodesMutex);
                }

                Serial.printf("[ACT]  Node #%d: %u actuator(s) registered\n", id, count);
                wsBroadcast(buildNodeActuatorSchemaJson(id));
                wsBroadcast(buildNodesJson());
                break;
            }

            case MSG_RFID_CONFIG_DATA: {
                if (pkt.len < (int)sizeof(MeshHeader) + 1) break;
                const uint8_t id = hdr->node_id;
                if (id == 0 || id >= nextId) break;
                if (nodes[id].mac[0] == 0) break;

                auto* rd = (MsgRfidConfigData*)pkt.data;
                uint8_t count = rd->count;
                if (count > RFID_MAX_SLOTS) count = RFID_MAX_SLOTS;
                const size_t expectedLen = sizeof(MeshHeader) + 1 + count * sizeof(RfidSlotDef);
                if ((size_t)pkt.len < expectedLen) break;

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    memset(nodes[id].rfidSlots, 0, sizeof(nodes[id].rfidSlots));
                    for (uint8_t i = 0; i < count; i++) {
                        const RfidSlotDef& slot = rd->slots[i];
                        if (slot.slot >= RFID_MAX_SLOTS) continue;
                        GatewayRfidSlot& dst = nodes[id].rfidSlots[slot.slot];
                        dst.enabled = slot.enabled != 0;
                        dst.uidLen = (slot.uid_len <= RFID_UID_MAX_LEN) ? slot.uid_len : 0;
                        dst.relayMask = slot.relay_mask;
                        if (dst.uidLen > 0) memcpy(dst.uid, slot.uid, dst.uidLen);
                    }
                    nodes[id].rfidConfigReady = true;
                    nodes[id].lastSeen = millis();
                    nodes[id].online = true;
                    xSemaphoreGive(nodesMutex);
                }

                Serial.printf("[RFID]  Node #%d: RFID config updated (%u slot payload entries)\n", id, count);
                wsBroadcast(buildNodeRfidConfigJson(id));
                wsBroadcast(buildNodesJson());
                break;
            }

            case MSG_RFID_SCAN_EVENT: {
                if (pkt.len < (int)sizeof(MsgRfidScanEvent)) break;
                const uint8_t id = hdr->node_id;
                if (id == 0 || id >= nextId) break;
                if (nodes[id].mac[0] == 0) break;
                auto* ev = (MsgRfidScanEvent*)pkt.data;
                const uint8_t uidLen = (ev->uid_len <= RFID_UID_MAX_LEN) ? ev->uid_len : 0;

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    nodes[id].lastRfidUidLen = uidLen;
                    memset(nodes[id].lastRfidUid, 0, sizeof(nodes[id].lastRfidUid));
                    if (uidLen > 0) memcpy(nodes[id].lastRfidUid, ev->uid, uidLen);
                    nodes[id].lastRfidMatchedSlot = (ev->matched_slot == 0xFF) ? -1 : (int8_t)ev->matched_slot;
                    nodes[id].lastRfidAppliedMask = ev->applied_relay_mask;
                    nodes[id].lastRfidSeenAt = millis();
                    nodes[id].lastSeen = nodes[id].lastRfidSeenAt;
                    nodes[id].online = true;
                    const uint8_t nextMask = (uint8_t)(ev->applied_relay_mask & 0xFF);
                    if (nodes[id].actuatorMask != nextMask) {
                        nodes[id].actuatorMask = nextMask;
                        markNodeRegistryDirty();
                    }
                    xSemaphoreGive(nodesMutex);
                }

                Serial.printf("[RFID]  Node #%d scanned UID=%s slot=%d mask=0x%02X\n",
                              id,
                              bytesToHexString(ev->uid, uidLen).c_str(),
                              (ev->matched_slot == 0xFF) ? -1 : ev->matched_slot,
                              ev->applied_relay_mask);
                wsBroadcast(buildRfidScanEventJson(id));
                wsBroadcast(buildNodesJson());
                break;
            }

            case MSG_NODE_OTA_STATUS: {
                if (pkt.len < (int)sizeof(MsgNodeOtaStatus)) break;
                auto* st = (MsgNodeOtaStatus*)pkt.data;
                const uint8_t id = hdr->node_id;
                if (id == 0 || id >= nextId) break;
                if (!nodeOtaJob.active || id != nodeOtaJob.nodeId || st->session_id != nodeOtaJob.sessionId) break;

                nodeOtaJob.nodeAccepted = true;
                nodeOtaJob.lastActivityAt = millis();
                if (st->phase == NODE_OTA_ACCEPTED) {
                    setNodeOtaJobMessage("Node accepted OTA request. Switching to OTA Wi-Fi...", 72);
                } else if (st->phase == NODE_OTA_ERROR) {
                    setNodeOtaJobError(st->message[0] ? st->message : "Node rejected the OTA request.");
                } else {
                    setNodeOtaJobMessage(st->message[0] ? st->message : "Node OTA status updated.",
                                         st->progress);
                }
                break;
            }

            case MSG_HEARTBEAT: {
                if (pkt.len < (int)sizeof(MsgHeartbeat)) break;
                auto* hb  = (MsgHeartbeat*)pkt.data;
                uint8_t id = hdr->node_id;
                if (id == 0 || id >= nextId) break;

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    nodes[id].lastSeen = millis();
                    nodes[id].online   = true;
                    nodes[id].uptime   = hb->uptime_sec;
                    xSemaphoreGive(nodesMutex);
                }
                break;
            }

            // Node-initiated disconnect (Pairing Button held 5-seconds on node)
            case MSG_UNPAIR_CMD: {
                uint8_t id = findNodeByMac(pkt.mac);
                if (id == 0) break;
                Serial.printf("[PAIR]  Node #%d self-disconnected\n", id);
                disconnectNode(id);
                break;
            }

            // Settings schema received from node
            case MSG_SETTINGS_DATA: {
                // Minimum: header(3) + count(1) = 4 bytes
                if (pkt.len < 4) break;
                uint8_t id = hdr->node_id;
                if (id == 0 || id >= nextId) break;
                if (nodes[id].mac[0] == 0) break;

                auto* sd    = (MsgSettingsData*)pkt.data;
                uint8_t cnt = sd->count;
                if (cnt > NODE_MAX_SETTINGS) cnt = NODE_MAX_SETTINGS;

                // Validate that the packet is long enough for the declared count
                size_t expectedLen = sizeof(MeshHeader) + 1 + cnt * sizeof(SettingDef);
                if ((size_t)pkt.len < expectedLen) {
                    Serial.printf("[CFG]  Node #%d settings packet too short (%d < %d)\n",
                                  id, pkt.len, (int)expectedLen);
                    break;
                }

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    nodes[id].settingsCount = cnt;
                    for (uint8_t i = 0; i < cnt; i++)
                        nodes[id].settings[i] = sd->settings[i];
                    xSemaphoreGive(nodesMutex);
                }

                Serial.printf("[CFG]  Node #%d: %d settings received\n", id, cnt);
                if (gwLedEnabled) {
                    flashGwLed(gwLed.Color(0, 0, 255), 100);  // brief blue = config rx
                }
                 else {
                    setGwLed(0); // turn off immediately
                }
                // Push updated settings to authenticated WS clients
                wsBroadcast(buildNodeSettingsJson(id));
                // Also push nodes update so settings_ready flag refreshes in the table
                wsBroadcast(buildNodesJson());
                break;
            }

            // Sensor schema received from node
            case MSG_SENSOR_SCHEMA: {
                // Minimum: header(3) + count(1) = 4 bytes
                if (pkt.len < 4) break;
                uint8_t id = hdr->node_id;
                if (id == 0 || id >= nextId) break;
                if (nodes[id].mac[0] == 0) break;

                auto* ss    = (MsgSensorSchema*)pkt.data;
                uint8_t cnt = ss->count;
                if (cnt > NODE_MAX_SENSORS) cnt = NODE_MAX_SENSORS;

                // Validate packet is large enough for the declared schema entries
                size_t expectedLen = sizeof(MeshHeader) + 1 + cnt * sizeof(SensorDef);
                if ((size_t)pkt.len < expectedLen) {
                    Serial.printf("[SENS] Node #%d schema pkt too short (%d < %d)\n",
                                  id, pkt.len, (int)expectedLen);
                    break;
                }

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    nodes[id].sensorCount = cnt;
                    for (uint8_t i = 0; i < cnt; i++)
                        nodes[id].sensorSchema[i] = ss->sensors[i];
                    xSemaphoreGive(nodesMutex);
                }

                Serial.printf("[SENS] Node #%d: %d sensor(s) registered", id, cnt);
                for (uint8_t i = 0; i < cnt; i++)
                    Serial.printf("  [%d]\"%s\"(%s)", ss->sensors[i].id,
                                  ss->sensors[i].label, ss->sensors[i].unit);
                Serial.println();
                
                if (gwLedEnabled) {
                    flashGwLed(gwLed.Color(0, 80, 255), 100);  // brief blue = schema rx
                }
                 else {
                    setGwLed(0); // turn off immediately
                }
                // Push updated schema to connected WS clients
                wsBroadcast(buildNodeSensorSchemaJson(id));
                // Also push nodes update so sensor_schema_ready flag refreshes
                wsBroadcast(buildNodesJson());
                break;
            }

            default: break;
        }
    }
}

// *****************************************************************************
//  WebSocket event handler
// *****************************************************************************
static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg,
                      uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("[WS]  Client #%u  %s\n",
                          client->id(), client->remoteIP().toString().c_str());
            if (!credentialsSet()) {
                // Open access - auto-authenticate and send initial data
                authWsClients.insert(client->id());
                client->text(buildMetaJson());
                client->text(buildNodesJson());
                client->text(buildDiscoveredJson());
                client->text(buildNodeOtaJson());
            } else {
                // Credentials are set - client must authenticate first
                client->text("{\"type\":\"auth_required\"}");
            }
            break;

        case WS_EVT_DISCONNECT:
            Serial.printf("[WS]  Client #%u disconnected\n", client->id());
            authWsClients.erase(client->id());
            break;

        case WS_EVT_DATA: {
            auto* info = (AwsFrameInfo*)arg;
            if (!info->final || info->index != 0 ||
                info->len != len || info->opcode != WS_TEXT) break;

            data[len] = '\0';  // safe: AsyncWebSocket always allocates len+1
            JsonDocument doc;
            if (deserializeJson(doc, (char*)data, len) != DeserializationError::Ok) break;

            const char* msgType = doc["type"] | "";

            // Auth message - must be processed before the auth guard
            if (strcmp(msgType, "auth") == 0) {
                if (!credentialsSet()) {
                    authWsClients.insert(client->id());
                    client->text("{\"type\":\"auth_ok\"}");
                    client->text(buildMetaJson());
                    client->text(buildNodesJson());
                    client->text(buildDiscoveredJson());
                    client->text(buildNodeOtaJson());
                    break;
                }
                const char* tok = doc["token"] | "";
                bool valid = false;
                if (strlen(sessionToken)  > 0 && strcmp(tok, sessionToken)  == 0) valid = true;
                if (strlen(rememberToken) > 0 && strcmp(tok, rememberToken) == 0) valid = true;
                if (valid) {
                    authWsClients.insert(client->id());
                    client->text("{\"type\":\"auth_ok\"}");
                    client->text(buildMetaJson());
                    client->text(buildNodesJson());
                    client->text(buildDiscoveredJson());
                    client->text(buildNodeOtaJson());
                    Serial.printf("[AUTH]  WS client #%u authenticated\n", client->id());
                } else {
                    client->text("{\"type\":\"auth_fail\"}");
                    Serial.printf("[AUTH]  WS client #%u auth failed\n", client->id());
                }
                break;
            }

            // Guard: reject unauthenticated commands
            if (!isWsAuthenticated(client->id())) {
                client->text("{\"type\":\"auth_required\"}");
                break;
            }

            // Actuator command
            if (strcmp(msgType, "actuator_cmd") == 0) {
                uint8_t nodeId     = doc["node_id"]     | 0;
                uint8_t actuatorId = doc["actuator_id"] | 0;
                uint8_t state      = doc["state"]       | 0;
                bool ok = sendActuatorCmd(nodeId, actuatorId, state);
                if (ok) {
                    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        const uint8_t prevMask = nodes[nodeId].actuatorMask;
                        if (state) nodes[nodeId].actuatorMask |= (1u << actuatorId);
                        else        nodes[nodeId].actuatorMask &= ~(1u << actuatorId);
                        if (nodes[nodeId].actuatorMask != prevMask) {
                            markNodeRegistryDirty();
                        }
                        xSemaphoreGive(nodesMutex);
                    }
                    wsBroadcast(buildNodesJson());
                }

            // Pair command (user clicked "Connect")
            } else if (strcmp(msgType, "pair_cmd") == 0) {
                const char* macStr = doc["mac"] | "";
                uint8_t mac[6];
                if (!parseMac(macStr, mac)) break;

                // Already paired? Don't start the pairing process if the MAC 
                // is already registered to avoid confusion from multiple nodes 
                // with the same MAC in pairing mode, which can happen if you 
                // have multiple nodes in pairing mode at once or if a node fails 
                // to clean up its peer entry on the gateway after a disconnect.
                if (findNodeByMac(mac) != 0) break;

                if (peekFreeSlot() == 0) {
                    Serial.printf("[PAIR]  Rejecting pair for %s - max nodes reached (%u/%u)\n",
                                  macStr, countRegisteredNodes(), MESH_MAX_NODES);
                    client->text(buildPairCapacityFullJson(macStr));
                    break;
                }

                // Always register peer with channel=0 (= gateway's current WiFi
                // channel). The gateway is STA-locked to wifiChannel and ESP-NOW
                // will reject any send to a peer registered on a different channel.
                // tx_channel from the beacon is only used for logging; the node
                // will cycle back to ch1 within its scan loop and receive PAIR_CMD.
                if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
                addPeer(mac, 0);

                // Start retry loop
                pendingPair.active      = true;
                memcpy(pendingPair.mac, mac, 6);
                pendingPair.startedAt   = millis();
                pendingPair.lastAttempt = 0;  // send on next loop iteration

                Serial.printf("[PAIR]  Initiating pair with %s\n", macStr);

            // Rename node
            } else if (strcmp(msgType, "rename_node") == 0) {
                uint8_t nodeId = doc["node_id"] | 0;
                const char* newName = doc["name"] | "";
                if (nodeId == 0 || nodeId >= nextId) break;
                if (nodes[nodeId].mac[0] == 0) break;
                if (strlen(newName) > 0 && strlen(newName) < MESH_NODE_NAME_LEN) {
                    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        strncpy(nodes[nodeId].name, newName, sizeof(nodes[nodeId].name) - 1);
                        nodes[nodeId].name[sizeof(nodes[nodeId].name) - 1] = '\0';
                        xSemaphoreGive(nodesMutex);
                    }
                    Serial.printf("[MESH] Node #%d renamed to \"%s\"\n", nodeId, newName);
                    wsBroadcast(buildNodesJson());
                }

            }
            // Toggle gateway LED (used for pairing feedback and other status)
            else if (strcmp(msgType, "gw_led_toggle") == 0) {

                bool state = doc["state"] | 0;

                if (state) {
                    setGwLed(gwLed.Color(0, 255, 120));   // normal status color
                    saveGwLedState(true);
                } else {
                    setGwLed(0); // OFF
                    saveGwLedState(false);
                }
            }
            // Unpair command (user clicked "Disconnect")
            else if (strcmp(msgType, "unpair_cmd") == 0) {
                uint8_t nodeId = doc["node_id"] | 0;
                if (nodeId == 0 || nodeId >= nextId) break;
                if (nodes[nodeId].mac[0] == 0) break;

                sendUnpairCmd(nodes[nodeId].mac, nodeId);
                delay(80);  // brief wait for send before removing peer
                disconnectNode(nodeId);
            }
            // Reboot a specific node (from dashboard command)
            else if (strcmp(msgType, "reboot_node") == 0) {
                uint8_t nodeId = doc["node_id"] | 0;
                if (nodeId == 0 || nodeId >= nextId) break;
                if (nodes[nodeId].mac[0] == 0) break;
                if (!nodes[nodeId].online) break;  // pointless to send if offline

                sendRebootCmd(nodes[nodeId].mac, nodeId);
                Serial.printf("[MESH] Reboot command sent to node #%d\n", nodeId);
                if (gwLedEnabled) {
                    flashGwLed(gwLed.Color(255, 165, 0), 200);  // orange pulse
                }
                else {
                    setGwLed(0); // turn off immediately
                }

            // Reboot the gateway itself
            } else if (strcmp(msgType, "reboot_gw") == 0) {
                Serial.println("[GW]  Reboot requested via dashboard.");
                ws.textAll("{\"type\":\"gw_rebooting\"}");  // notify all clients
                ws.cleanupClients();
                delay(150);  // allow WS frame to flush before restart
                ESP.restart();

            // Update AP SSID / password
            } else if (strcmp(msgType, "set_ap_config") == 0) {
                const char* newSsid = doc["ssid"]     | "";
                const char* newPass = doc["password"] | "";
                size_t ssidLen = strlen(newSsid);
                size_t passLen = strlen(newPass);

                if (ssidLen < 2 || ssidLen > 32) {
                    client->text("{\"type\":\"ap_config_ack\",\"ok\":false,"
                                 "\"err\":\"SSID must be 2\\u201332 characters\"}");
                    break;
                }
                if (passLen > 0 && passLen < 8) {
                    client->text("{\"type\":\"ap_config_ack\",\"ok\":false,"
                                 "\"err\":\"Password must be 8+ chars or blank (open AP)\"}");
                    break;
                }
                saveApConfig(newSsid, newPass);
                client->text("{\"type\":\"ap_config_ack\",\"ok\":true}");

            // Set web interface login credentials
            } else if (strcmp(msgType, "set_web_credentials") == 0) {
                const char* newUser = doc["username"] | "";
                const char* newPass = doc["password"] | "";
                size_t uLen = strlen(newUser);
                size_t pLen = strlen(newPass);

                if (uLen < 1 || uLen > 32) {
                    client->text("{\"type\":\"web_creds_ack\",\"ok\":false,"
                                 "\"err\":\"Username must be 1\\u201332 characters\"}");
                    break;
                }
                if (pLen < 4 || pLen > 64) {
                    client->text("{\"type\":\"web_creds_ack\",\"ok\":false,"
                                 "\"err\":\"Password must be 4\\u201364 characters\"}");
                    break;
                }

                char newHash[65];
                computeSha256Hex(newPass, newHash);
                saveWebCredentials(newUser, newHash);

                // Invalidate ALL existing sessions immediately
                memset(sessionToken, 0, sizeof(sessionToken));
                authWsClients.clear();

                // Notify clients: ack first, then kick everyone to login
                ws.textAll("{\"type\":\"web_creds_ack\",\"ok\":true}");
                delay(60);
                ws.textAll("{\"type\":\"session_expired\"}");

            // Trigger WiFiManager portal (change home-router network)
            } else if (strcmp(msgType, "start_wifi_portal") == 0) {
                Serial.println("[GW]  WiFi portal requested via dashboard.");
                ws.textAll("{\"type\":\"gw_portal_starting\"}");
                ws.cleanupClients();
                delay(200);
                {
                    WiFiManager wm;
                    wm.resetSettings();  // wipe saved STA credentials -> portal opens on boot
                }
                delay(100);
                ESP.restart();

            // Node settings: request current schema from node
            } else if (strcmp(msgType, "node_settings_get") == 0) {
                uint8_t nodeId = doc["node_id"] | 0;
                if (nodeId == 0 || nodeId >= nextId) break;
                if (nodes[nodeId].mac[0] == 0) break;

                if (nodes[nodeId].settingsCount > 0) {
                    // Already have schema - respond immediately to this client only
                    client->text(buildNodeSettingsJson(nodeId));
                } else {
                    // Re-request from node
                    if (nodes[nodeId].online)
                        sendSettingsGet(nodes[nodeId].mac, nodeId, nodes[nodeId].type);
                    // Respond with empty settings until node replies
                    client->text(buildNodeSettingsJson(nodeId));
                }

            // Sensor schema: request current schema from node
            } else if (strcmp(msgType, "node_sensor_schema_get") == 0) {
                uint8_t nodeId = doc["node_id"] | 0;
                if (nodeId == 0 || nodeId >= nextId) break;
                if (nodes[nodeId].mac[0] == 0) break;

                if (nodes[nodeId].sensorCount > 0) {
                    // Already cached - serve immediately from RAM
                    client->text(buildNodeSensorSchemaJson(nodeId));
                } else {
                    // Not yet received - re-request from node if it is online
                    if (nodes[nodeId].online)
                        sendSensorSchemaGet(nodes[nodeId].mac, nodeId, nodes[nodeId].type);
                    // Respond with empty schema for now; MSG_SENSOR_SCHEMA handler
                    // will broadcast the real schema when the node replies.
                    client->text(buildNodeSensorSchemaJson(nodeId));
                }

            } else if (strcmp(msgType, "node_actuator_schema_get") == 0) {
                uint8_t nodeId = doc["node_id"] | 0;
                if (nodeId == 0 || nodeId >= nextId) break;
                if (nodes[nodeId].mac[0] == 0) break;
                if (!(nodes[nodeId].capabilities & NODE_CAP_ACTUATORS)) break;

                if (nodes[nodeId].actuatorCount > 0) {
                    client->text(buildNodeActuatorSchemaJson(nodeId));
                } else {
                    if (nodes[nodeId].online)
                        sendActuatorSchemaGet(nodes[nodeId].mac, nodeId, nodes[nodeId].type);
                    client->text(buildNodeActuatorSchemaJson(nodeId));
                }

            } else if (strcmp(msgType, "node_rfid_config_get") == 0) {
                uint8_t nodeId = doc["node_id"] | 0;
                if (nodeId == 0 || nodeId >= nextId) break;
                if (nodes[nodeId].mac[0] == 0) break;
                if (!(nodes[nodeId].capabilities & NODE_CAP_RFID)) break;

                if (!nodes[nodeId].rfidConfigReady && nodes[nodeId].online) {
                    sendRfidConfigGet(nodes[nodeId].mac, nodeId, nodes[nodeId].type);
                }
                client->text(buildNodeRfidConfigJson(nodeId));

            // Node settings: apply a single setting change
            } else if (strcmp(msgType, "node_settings_set") == 0) {
                uint8_t nodeId    = doc["node_id"]    | 0;
                uint8_t settingId = doc["setting_id"] | 0;
                int16_t value     = (int16_t)(doc["value"]   | 0);

                if (nodeId == 0 || nodeId >= nextId) break;
                if (nodes[nodeId].mac[0] == 0) break;
                if (!nodes[nodeId].online) break;

                // Optimistically update our cached value so the UI snaps immediately
                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    for (uint8_t i = 0; i < nodes[nodeId].settingsCount; i++) {
                        if (nodes[nodeId].settings[i].id == settingId) {
                            nodes[nodeId].settings[i].current = value;
                            break;
                        }
                    }
                    xSemaphoreGive(nodesMutex);
                }
                sendSettingsSet(nodes[nodeId].mac, nodeId, nodes[nodeId].type,
                                settingId, value);
                Serial.printf("[CFG]  Node #%d setting %d -> %d\n", nodeId, settingId, value);
                wsBroadcast(buildNodeSettingsJson(nodeId));

            } else if (strcmp(msgType, "node_rfid_config_set") == 0) {
                uint8_t nodeId = doc["node_id"] | 0;
                uint8_t slotIndex = doc["slot"] | 0xFF;
                bool enabled = doc["enabled"] | false;
                const char* uidText = doc["uid"] | "";
                uint16_t relayMask = (uint16_t)(doc["relay_mask"] | 0);

                if (nodeId == 0 || nodeId >= nextId || nodes[nodeId].mac[0] == 0) {
                    client->text(buildRfidConfigAckJson(nodeId, false, "Invalid node"));
                    break;
                }
                if (!(nodes[nodeId].capabilities & NODE_CAP_RFID)) {
                    client->text(buildRfidConfigAckJson(nodeId, false, "Selected node does not support RFID actions"));
                    break;
                }
                if (slotIndex >= RFID_MAX_SLOTS) {
                    client->text(buildRfidConfigAckJson(nodeId, false, "Invalid RFID slot"));
                    break;
                }
                if (!nodes[nodeId].online) {
                    client->text(buildRfidConfigAckJson(nodeId, false, "Node must be online to update RFID actions"));
                    break;
                }

                MsgRfidConfigSet msg{};
                msg.hdr.type = MSG_RFID_CONFIG_SET;
                msg.hdr.node_id = nodeId;
                msg.hdr.node_type = nodes[nodeId].type;
                msg.slot.slot = slotIndex;
                msg.slot.enabled = enabled ? 1 : 0;
                msg.slot.relay_mask = relayMask;

                if (enabled) {
                    if (!parseRfidUidHex(uidText, msg.slot.uid, &msg.slot.uid_len)) {
                        client->text(buildRfidConfigAckJson(nodeId, false, "RFID UID must be 4, 7, or 10 bytes of hexadecimal data"));
                        break;
                    }
                }

                if (esp_now_send(nodes[nodeId].mac, (uint8_t*)&msg, sizeof(msg)) != ESP_OK) {
                    client->text(buildRfidConfigAckJson(nodeId, false, "Failed to send RFID configuration to node"));
                    break;
                }

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    GatewayRfidSlot& slot = nodes[nodeId].rfidSlots[slotIndex];
                    memset(&slot, 0, sizeof(slot));
                    slot.enabled = enabled;
                    slot.uidLen = msg.slot.uid_len;
                    slot.relayMask = relayMask;
                    if (slot.uidLen > 0) memcpy(slot.uid, msg.slot.uid, slot.uidLen);
                    nodes[nodeId].rfidConfigReady = true;
                    xSemaphoreGive(nodesMutex);
                }

                wsBroadcast(buildNodeRfidConfigJson(nodeId));
                client->text(buildRfidConfigAckJson(nodeId, true));

            } else if (strcmp(msgType, "relay_labels_set") == 0) {
                uint8_t nodeId = doc["node_id"] | 0;
                if (nodeId == 0 || nodeId >= nextId) {
                    client->text(buildRelayLabelsAckJson(nodeId, false, "Invalid node"));
                    break;
                }
                if (nodes[nodeId].mac[0] == 0) {
                    client->text(buildRelayLabelsAckJson(nodeId, false, "Node not found"));
                    break;
                }
                if (!(nodes[nodeId].capabilities & NODE_CAP_ACTUATORS)) {
                    client->text(buildRelayLabelsAckJson(nodeId, false, "Relay labels only apply to actuator-capable nodes"));
                    break;
                }

                JsonArrayConst labels = doc["labels"].as<JsonArrayConst>();
                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    for (uint8_t i = 0; i < NODE_MAX_ACTUATORS; i++) {
                        const char* raw = labels.isNull() ? "" : (labels[i] | "");
                        sanitizeRelayLabel(raw, i, nodes[nodeId].relayLabels[i], RELAY_LABEL_MAX_LEN);
                    }
                    xSemaphoreGive(nodesMutex);
                }

                saveRelayLabelsForNode(nodeId);
                wsBroadcast(buildNodesJson());
                client->text(buildRelayLabelsAckJson(nodeId, true));

            // Factory reset: wipe ALL config and paired nodes, then reboot
            } else if (strcmp(msgType, "factory_reset") == 0) {
                Serial.println("[GW]  Factory reset requested via dashboard.");
                ws.textAll("{\"type\":\"gw_factory_reset\"}");
                ws.cleanupClients();
                delay(200);
                {
                    WiFiManager wm;
                    wm.resetSettings();  // wipe saved WiFi credentials
                }
                {
                    Preferences prefs;
                    prefs.begin("gwconfig", false);
                    prefs.clear();        // wipe custom AP name / password
                    prefs.end();
                }
                {
                    Preferences prefs;
                    prefs.begin("gwauth", false);
                    prefs.clear();        // wipe web interface credentials
                    prefs.end();
                    memset(webUsername,   0, sizeof(webUsername));
                    memset(webPassHash,   0, sizeof(webPassHash));
                    memset(rememberToken, 0, sizeof(rememberToken));
                    memset(sessionToken,  0, sizeof(sessionToken));
                    authWsClients.clear();
                }
                {
                    Preferences prefs;
                    prefs.begin("gwnodes", false);
                    prefs.clear();        // wipe paired node registry
                    prefs.end();
                }
                {
                    Preferences prefs;
                    prefs.begin("gwrelay", false);
                    prefs.clear();        // wipe saved relay label assignments
                    prefs.end();
                }
                delay(100);
                ESP.restart();
            }
            break;
        }

        case WS_EVT_ERROR:
            Serial.printf("[WS]  Error on client #%u\n", client->id());
            break;

        default: break;
    }
}

// *****************************************************************************
//  Web server routes
// *****************************************************************************
static void setupRoutes() {
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // Static files served without auth - the JS login overlay handles access control.
    server.serveStatic("/", LittleFS, "/")
          .setDefaultFile("index.html")
          .setCacheControl("no-store, no-cache, must-revalidate, max-age=0");

    // /api/auth_check - used by the client on initial page load to determine if credentials are set
    // and if the user is already authenticated (e.g. via remember me)
    server.on("/api/auth_check", HTTP_GET, [](AsyncWebServerRequest* req) {
        bool authed   = isHttpAuthenticated(req);
        bool credsSet = credentialsSet();
        String body = String("{\"authenticated\":") + (authed   ? "true" : "false") +
                      ",\"credentials_set\":"        + (credsSet ? "true" : "false") + "}";
        req->send(200, "application/json", body);
    });

    // /api/login - accepts username/password, returns session token (and remember token if requested)
    server.on("/api/login", HTTP_POST,
        [](AsyncWebServerRequest*) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                req->send(400, "application/json", R"({"ok":false,"error":"Invalid JSON"})");
                return;
            }
            const char* user  = doc["username"] | "";
            const char* pass  = doc["password"] | "";
            bool        remem = doc["remember"] | false;

            if (!credentialsSet()) {
                // No credentials configured - open access
                req->send(200, "application/json", R"({"ok":true,"token":"open"})");
                return;
            }

            char passHash[65];
            computeSha256Hex(pass, passHash);

            if (strcmp(user, webUsername) != 0 || strcmp(passHash, webPassHash) != 0) {
                Serial.printf("[AUTH]  Login FAIL for user \"%s\"\n", user);
                req->send(401, "application/json",
                          R"({"ok":false,"error":"Invalid username or password"})");
                return;
            }

            // Generate a fresh in-memory session token
            generateHexToken(sessionToken, 32);

            // Build JSON - always return session token for WS auth; include
            // the NVS remember token too when "remember me" was checked.
            String body = String("{\"ok\":true,\"token\":\"") + sessionToken + "\"";
            if (remem) body += String(",\"remember_token\":\"") + rememberToken + "\"";
            body += "}";

            AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", body);
            resp->addHeader("Set-Cookie",
                String("gwsession=") + sessionToken +
                "; HttpOnly; Path=/; SameSite=Strict");
            if (remem) {
                resp->addHeader("Set-Cookie",
                    String("gwremember=") + rememberToken +
                    "; Path=/; Max-Age=31536000; SameSite=Strict");
            } else {
                // Clear any stale remember cookie
                resp->addHeader("Set-Cookie",
                    "gwremember=; Path=/; Max-Age=0; SameSite=Strict");
            }
            req->send(resp);
            Serial.printf("[AUTH]  Login OK: \"%s\" (remember=%d)\n", user, (int)remem);
        }
    );

    // /api/logout - clears session and remember tokens, forces logout on all clients
    server.on("/api/logout", HTTP_POST, [](AsyncWebServerRequest* req) {
        memset(sessionToken, 0, sizeof(sessionToken));
        authWsClients.clear();
        AsyncWebServerResponse* resp =
            req->beginResponse(200, "application/json", R"({"ok":true})");
        resp->addHeader("Set-Cookie",
            "gwsession=; HttpOnly; Path=/; Max-Age=0; SameSite=Strict");
        resp->addHeader("Set-Cookie",
            "gwremember=; Path=/; Max-Age=0; SameSite=Strict");
        req->send(resp);
        Serial.println("[AUTH]  Logout");
        ws.textAll("{\"type\":\"session_expired\"}");
    });

    // Protected API routes - require valid session or remember token
    server.on("/api/nodes", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!isHttpAuthenticated(req)) {
            req->send(401, "application/json", R"({"error":"unauthorized"})"); return;
        }
        req->send(200, "application/json", buildNodesJson());
    });
    server.on("/api/meta", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!isHttpAuthenticated(req)) {
            req->send(401, "application/json", R"({"error":"unauthorized"})"); return;
        }
        req->send(200, "application/json", buildMetaJson());
    });
    server.on("/api/discovered", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!isHttpAuthenticated(req)) {
            req->send(401, "application/json", R"({"error":"unauthorized"})"); return;
        }
        req->send(200, "application/json", buildDiscoveredJson());
    });
    server.on("/api/actuator", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!isHttpAuthenticated(req)) {
                req->send(401, "application/json", R"({"error":"unauthorized"})");
            }
        },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data,
           size_t len, size_t, size_t) {
            if (!isHttpAuthenticated(req)) return;  // already replied above
            JsonDocument doc;
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                req->send(400, "application/json", R"({"error":"invalid JSON"})");
                return;
            }
            bool ok = sendActuatorCmd(
                (uint8_t)(doc["node_id"]     | 0),
                (uint8_t)(doc["actuator_id"] | 0),
                (uint8_t)(doc["state"]       | 0));
            req->send(200, "application/json", ok ? R"({"ok":true})" : R"({"ok":false})");
        }
    );
    server.on("/api/gateway/ota", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!isHttpAuthenticated(req)) {
                req->send(401, "application/json", R"({"ok":false,"error":"unauthorized"})");
                return;
            }

            auto* state = reinterpret_cast<GatewayOtaRequestState*>(req->_tempObject);
            if (!state || !state->started) {
                req->send(400, "application/json", R"({"ok":false,"error":"No firmware upload received."})");
            } else if (state->error[0] != '\0') {
                int status = (strcmp(state->error, "Another OTA upload is already in progress.") == 0) ? 409 : 400;
                JsonDocument doc;
                doc["ok"] = false;
                doc["error"] = state->error;
                String body;
                serializeJson(doc, body);
                req->send(status, "application/json", body);
            } else if (!state->ok) {
                req->send(500, "application/json", R"({"ok":false,"error":"Firmware update did not complete."})");
            } else {
                otaRebootPending = true;
                otaRebootAtMs = millis() + 1500;
                wsBroadcast("{\"type\":\"gw_rebooting\"}");

                JsonDocument doc;
                doc["ok"] = true;
                doc["rebooting"] = true;
                doc["version"] = getIncomingGatewayFirmwareVersion(state);
                doc["message"] = "Firmware flashed successfully. Gateway is rebooting.";
                String body;
                serializeJson(doc, body);
                req->send(200, "application/json", body);
            }

            delete state;
            req->_tempObject = nullptr;
            otaUploadBusy = false;
        },
        handleGatewayOtaUpload
    );
    server.on("/api/node/ota", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!isHttpAuthenticated(req)) {
                req->send(401, "application/json", R"({"ok":false,"error":"unauthorized"})");
                return;
            }

            auto* state = reinterpret_cast<NodeOtaUploadRequestState*>(req->_tempObject);
            const AsyncWebHeader* nodeHdr = req->getHeader("X-Node-Id");
            const uint8_t nodeId = nodeHdr ? (uint8_t)atoi(nodeHdr->value().c_str()) : 0;

            auto cleanup = [&]() {
                if (state) {
                    if (state->file) state->file.close();
                    delete state;
                    req->_tempObject = nullptr;
                }
                nodeOtaJob.uploadBusy = false;
            };

            if (!state || !state->started) {
                req->send(400, "application/json", R"({"ok":false,"error":"No node firmware upload received."})");
                cleanup();
                return;
            }

            if (state->error[0] != '\0') {
                JsonDocument doc;
                doc["ok"] = false;
                doc["error"] = state->error;
                String body;
                serializeJson(doc, body);
                req->send(400, "application/json", body);
                if (LittleFS.exists(NODE_OTA_FILE_PATH)) LittleFS.remove(NODE_OTA_FILE_PATH);
                cleanup();
                return;
            }

            if (!state->ok) {
                req->send(500, "application/json", R"({"ok":false,"error":"Node firmware upload did not complete."})");
                if (LittleFS.exists(NODE_OTA_FILE_PATH)) LittleFS.remove(NODE_OTA_FILE_PATH);
                cleanup();
                return;
            }

            if (nodeId == 0 || nodeId >= nextId || nodes[nodeId].mac[0] == 0) {
                req->send(400, "application/json", R"({"ok":false,"error":"Selected node is invalid."})");
                if (LittleFS.exists(NODE_OTA_FILE_PATH)) LittleFS.remove(NODE_OTA_FILE_PATH);
                cleanup();
                return;
            }

            if (!nodes[nodeId].online) {
                req->send(400, "application/json", R"({"ok":false,"error":"Selected node must be online before OTA can begin."})");
                if (LittleFS.exists(NODE_OTA_FILE_PATH)) LittleFS.remove(NODE_OTA_FILE_PATH);
                cleanup();
                return;
            }

            const NodeType uploadedType = roleToNodeType(state->incomingRole);
            if (uploadedType == 0 || uploadedType != nodes[nodeId].type) {
                JsonDocument doc;
                doc["ok"] = false;
                doc["error"] = String("Firmware type mismatch. Selected node expects ")
                    + nodeTypeToRole(nodes[nodeId].type) + " firmware.";
                String body;
                serializeJson(doc, body);
                req->send(400, "application/json", body);
                if (LittleFS.exists(NODE_OTA_FILE_PATH)) LittleFS.remove(NODE_OTA_FILE_PATH);
                cleanup();
                return;
            }

            if (nodes[nodeId].hw_config_id[0] == '\0') {
                Serial.printf("[NODE OTA]  Node #%d has no stored hardware config ID. "
                              "Reboot or re-pair the node so it can re-register with the "
                              "latest firmware metadata.\n", nodeId);
                req->send(400, "application/json",
                          R"({"ok":false,"error":"Selected node did not report a hardware configuration ID. If it is already on the latest firmware, reboot or re-pair the node once so the gateway can refresh its stored hardware ID; otherwise flash the latest node firmware first."})");
                if (LittleFS.exists(NODE_OTA_FILE_PATH)) LittleFS.remove(NODE_OTA_FILE_PATH);
                cleanup();
                return;
            }

            if (!hardwareConfigIdsMatch(state->incomingHwConfigId, nodes[nodeId].hw_config_id)) {
                JsonDocument doc;
                doc["ok"] = false;
                doc["error"] = String("Firmware hardware configuration mismatch. Selected node expects ")
                    + nodes[nodeId].hw_config_id + " but the uploaded firmware targets "
                    + state->incomingHwConfigId + ".";
                String body;
                serializeJson(doc, body);
                req->send(400, "application/json", body);
                if (LittleFS.exists(NODE_OTA_FILE_PATH)) LittleFS.remove(NODE_OTA_FILE_PATH);
                cleanup();
                return;
            }

            nodeOtaJob = NodeOtaJobState{};
            nodeOtaJob.nodeId = nodeId;
            nodeOtaJob.targetType = uploadedType;
            nodeOtaJob.stage = NODE_OTA_JOB_STAGED;
            nodeOtaJob.sessionId = esp_random();
            nodeOtaJob.imageSize = state->written;
            nodeOtaJob.imageCrc32 = state->crc32;
            nodeOtaJob.stageStartedAt = millis();
            nodeOtaJob.lastActivityAt = nodeOtaJob.stageStartedAt;
            strncpy(nodeOtaJob.version, state->incomingDisplayVersion, sizeof(nodeOtaJob.version) - 1);
            strncpy(nodeOtaJob.priorVersion, nodes[nodeId].fw_version, sizeof(nodeOtaJob.priorVersion) - 1);
            strncpy(nodeOtaJob.filename, state->filename, sizeof(nodeOtaJob.filename) - 1);
            buildNodeOtaCredentials();
            nodeOtaJob.active = true;
            setNodeOtaJobMessage(String("Node firmware uploaded for ") + nodes[nodeId].name + ". Preparing OTA helper...", 5);

            JsonDocument doc;
            doc["ok"] = true;
            doc["queued"] = true;
            doc["session_id"] = nodeOtaJob.sessionId;
            doc["version"] = nodeOtaJob.version;
            String body;
            serializeJson(doc, body);
            req->send(200, "application/json", body);
            cleanup();
            broadcastNodeOtaState();
        },
        handleNodeOtaUpload
    );
    server.onNotFound([](AsyncWebServerRequest* req) {
        Serial.printf("[HTTP] 404: %s\n", req->url().c_str());
        req->send(404, "text/plain", "Not found");
    });
}

// *****************************************************************************
//  SETUP
// *****************************************************************************
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n\n[BOOT] ESP32 Mesh Gateway");
    bootMs = millis();
    touchGatewayFirmwareMarkers();

    // Gateway Status LED Setup
    gwLed.begin();
    gwLed.setBrightness(60);
    loadGwLedState();
    if (gwLedEnabled) {
        setGwLed(gwLed.Color(255, 255, 255));   // white at boot
    } else {
        setGwLed(0); // OFF
    }
    delay(300);

    // RTOS objects
    nodesMutex = xSemaphoreCreateMutex();
    rxQueue    = xQueueCreate(RX_QUEUE_SIZE, sizeof(RxPacket));
    memset(nodes,      0, sizeof(nodes));
    memset(discovered, 0, sizeof(discovered));

    // LittleFS
    mountFilesystem();

    // WiFi credentials reset
    pinMode(RESET_BTN_PIN, INPUT_PULLUP);
    if (digitalRead(RESET_BTN_PIN) == LOW) {
        Serial.println("[WiFi] Clearing credentials...");
        WiFiManager wm;
        wm.resetSettings();
        delay(500);
    }

    // Load gateway config (AP name / password) from NVS
    loadApConfig();

    // Load web interface credentials from NVS
    loadWebCredentials();

    // WiFiManager
    {
        WiFiManager wm;
        wm.setTitle("ESP32 Mesh Gateway Setup");
        wm.setConfigPortalTimeout(180);
        wm.setHttpPort(8080);  // use port 8080 so WiFiManager never conflicts with AsyncWebServer on port 80
        Serial.println("[WiFi] Connecting (portal if needed)...");
        if (!wm.autoConnect(gwApSsid, gwApPassword)) {
            Serial.println("[WiFi] Failed - rebooting");
            delay(1000);
            ESP.restart();
        }
        wifiChannel = (uint8_t)WiFi.channel();
        Serial.printf("[WiFi] Connected: %s  IP: %s  Ch: %d\n",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), wifiChannel);
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    delay(1000);

    // ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] Init failed - rebooting");
        delay(1000);
        ESP.restart();
    }
    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onDataSent);

    // Add broadcast peer so we can receive beacons
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    addPeer(broadcast);

    // Restore paired nodes from NVS - must happen after ESP-NOW init so
    // addPeer() calls inside loadNodesFromNvs() succeed.
    loadNodesFromNvs();

    initCoprocLink();

    Serial.printf("[ESP-NOW] Ready - MAC: %s  Ch: %d\n",
                  WiFi.macAddress().c_str(), wifiChannel);

    // Web server
    setupRoutes();
    server.begin();
    Serial.printf("[HTTP]  Dashboard -> http://%s/\n", WiFi.localIP().toString().c_str());
    
    if (gwLedEnabled) {
        setGwLed(gwLed.Color(0, 0, 32));  // settle to dim blue
    }
    else {
        setGwLed(0); // OFF
    }
    Serial.println("[BOOT] Setup complete.\n");
}

// *****************************************************************************
//  LOOP
// *****************************************************************************
static unsigned long lastNodeBcast = 0;
static unsigned long lastMetaBcast = 0;
static unsigned long lastDiscClean = 0;
static unsigned long lastDiscBcast = 0;

void loop() {
    unsigned long now = millis();

    processRxQueue();
    processCoprocSerial();
    processCoprocHeartbeat(now);
    processNodeOtaJob(now);
    updateGwLed();

    if (otaRebootPending && now >= otaRebootAtMs) {
        otaRebootPending = false;
        Serial.println("[OTA]  Rebooting into updated gateway firmware.");
        delay(100);
        ESP.restart();
    }

    if (nodeRegistryDirty && (now - nodeRegistryDirtyAtMs) >= NODE_REGISTRY_FLUSH_MS) {
        saveNodesToNvs();
    }

    // Pending pair: retry PAIR_CMD until MSG_REGISTER arrives or timeout expires
    if (pendingPair.active) {
        if (now - pendingPair.startedAt > PAIR_CMD_TIMEOUT_MS) {
            pendingPair.active = false;
            Serial.println("[PAIR]  Timed out - no response from node.");
            wsBroadcast("{\"type\":\"pair_timeout\"}");
        } else if (now - pendingPair.lastAttempt >= PAIR_CMD_RETRY_MS) {
            pendingPair.lastAttempt = now;
            sendPairCmd(pendingPair.mac);
            Serial.printf("[PAIR]  Sending PAIR_CMD -> %02X:%02X:%02X:%02X:%02X:%02X\n",
                          pendingPair.mac[0], pendingPair.mac[1], pendingPair.mac[2],
                          pendingPair.mac[3], pendingPair.mac[4], pendingPair.mac[5]);
        }
    }

    // Periodic WS pushes
    if (now - lastNodeBcast >= WS_UPDATE_MS) {
        lastNodeBcast = now;
        if (ws.count() > 0) wsBroadcast(buildNodesJson());
    }
    if (now - lastMetaBcast >= WS_META_MS) {
        lastMetaBcast = now;
        if (ws.count() > 0) wsBroadcast(buildMetaJson());
    }

    // Periodic discovered list push (keeps clients in sync)
    if (now - lastDiscBcast >= WS_UPDATE_MS) {
        lastDiscBcast = now;
        if (ws.count() > 0) wsBroadcast(buildDiscoveredJson());
    }

    // Periodic discovered list cleanup (removes stale entries 
    // for nodes that disappeared without cleanly unregistering)
    if (now - lastDiscClean >= 2000) {
        lastDiscClean = now;
        if (cleanupDiscovered() && ws.count() > 0)
            wsBroadcast(buildDiscoveredJson());
    }

    ws.cleanupClients();
    delay(10);
}












