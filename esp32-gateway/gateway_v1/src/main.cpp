/**
    * @file [main.cpp]
    * @brief Main source file for the ESP32 Mesh Gateway firmware
    * @version 1.8.3
    * @author Mrinal (@atechofficials)
 */
#define FW_VERSION "1.8.3"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "mesh_protocol.h"
#include <Preferences.h>
#include "mbedtls/sha256.h"
#include <set>

// Configuration
#define RESET_BTN_PIN    0
#define AP_SSID_DEFAULT  "ESP32-Mesh-Gateway"
#define AP_PASS_DEFAULT  "meshsetup"
#define WEB_PORT         80
#define RELAY_LABEL_MAX_LEN 25

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

// Node Registry
struct NodeRecord {
    uint8_t       mac[6];
    NodeType      type;
    char          name[16];
    char          fw_version[8];  // reported by node in MSG_REGISTER
    unsigned long lastSeen;
    bool          online;
    uint32_t      uptime;
    uint8_t actuatorMask;
    // Per-node dynamic settings schema (populated by MSG_SETTINGS_DATA)
    uint8_t       settingsCount;                     // 0 = not yet received
    SettingDef    settings[NODE_MAX_SETTINGS];        // schema + current values
    // Per-node dynamic sensor schema (populated by MSG_SENSOR_SCHEMA)
    // Indexed by position; sensorSchema[j] and sensorValues[j] are always parallel.
    // sensorCount == 0 means schema has not yet been received from this node.
    uint8_t       sensorCount;                       // 0 = schema not yet received
    SensorDef     sensorSchema[NODE_MAX_SENSORS];    // descriptor for each sensor channel
    float         sensorValues[NODE_MAX_SENSORS];    // last received value per channel
    char          relayLabels[NODE_MAX_ACTUATORS][RELAY_LABEL_MAX_LEN];
};
static NodeRecord nodes[MESH_MAX_NODES + 1];
static uint8_t    nextId = 1;

// Discovered (beaconing) nodes - not yet paired
struct DiscoveredNode {
    uint8_t       mac[6];
    char          name[16];
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

struct RelayLabelRecord {
    char labels[NODE_MAX_ACTUATORS][RELAY_LABEL_MAX_LEN];
};

static void loadRelayLabelsForNode(uint8_t nodeId) {
    if (nodeId == 0 || nodeId > MESH_MAX_NODES) return;
    resetRelayLabels(nodes[nodeId].relayLabels);
    if (nodes[nodeId].mac[0] == 0 && nodes[nodeId].mac[1] == 0) return;

    RelayLabelRecord rec{};
    char key[13];
    buildRelayLabelPrefKey(nodes[nodeId].mac, key, sizeof(key));

    Preferences prefs;
    prefs.begin("gwrelay", true);
    size_t got = prefs.getBytes(key, &rec, sizeof(rec));
    prefs.end();

    if (got != sizeof(rec)) return;

    for (uint8_t i = 0; i < NODE_MAX_ACTUATORS; i++) {
        sanitizeRelayLabel(rec.labels[i], i, nodes[nodeId].relayLabels[i], RELAY_LABEL_MAX_LEN);
    }
}

static void saveRelayLabelsForNode(uint8_t nodeId) {
    if (nodeId == 0 || nodeId > MESH_MAX_NODES) return;
    if (nodes[nodeId].mac[0] == 0 && nodes[nodeId].mac[1] == 0) return;

    RelayLabelRecord rec{};
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

// Returns true if the HTTP request carries a valid session or remember cookie.
static bool isHttpAuthenticated(AsyncWebServerRequest* req) {
    if (!credentialsSet()) {
        // For Debugging: print the authentication attempt to the Serial console
        Serial.printf("[AUTH]  HTTP auth not required for request to %s\n", req->url().c_str());
        return true;
    }
    if (strlen(sessionToken) > 0) {
        String v = getCookieValue(req, "gwsession");
        if (v.length() > 0 && v.equals(sessionToken)) {
            // For Debugging: print the authentication attempt to the Serial console
            Serial.printf("[AUTH]  HTTP auth OK for request to %s\n", req->url().c_str());
            return true;
        }
    }
    if (strlen(rememberToken) > 0) {
        String v = getCookieValue(req, "gwremember");
        if (v.length() > 0 && v.equals(rememberToken)) {
            // For Debugging: print the authentication attempt to the Serial console
            Serial.printf("[AUTH]  HTTP auth OK for request to %s\n", req->url().c_str());
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
struct NodeNvsRecord {
    uint8_t  mac[6];
    NodeType type;
    char     name[16];
    char     fw_version[8];
};  // 31 bytes per node

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
        NodeNvsRecord rec;
        memcpy(rec.mac,        nodes[i].mac,        6);
        rec.type = nodes[i].type;
        memcpy(rec.name,       nodes[i].name,       16);
        memcpy(rec.fw_version, nodes[i].fw_version, 8);
        prefs.putBytes(key, &rec, sizeof(rec));
    }
    prefs.end();
    // For Debugging: print the saved nodes to the Serial console
    Serial.printf("[MESH] Saved %d nodes to NVS. nextId=%d\n", nextId - 1, nextId);
}

// Called once from setup(), after ESP-NOW is initialised so addPeer() works.
static void loadNodesFromNvs() {
    Preferences prefs;
    prefs.begin("gwnodes", true);
    uint8_t savedNextId = prefs.getUChar("nextid", 1);
    if (savedNextId <= 1) { prefs.end(); return; }

    uint8_t restored = 0;
    for (uint8_t i = 1; i < savedNextId; i++) {
        char key[5];
        snprintf(key, sizeof(key), "n%d", i);
        NodeNvsRecord rec;
        if (prefs.getBytes(key, &rec, sizeof(rec)) != sizeof(rec)) continue;
        if (rec.mac[0] == 0 && rec.mac[1] == 0 && rec.mac[2] == 0) continue;

        memcpy(nodes[i].mac,        rec.mac,        6);
        nodes[i].type = rec.type;
        memcpy(nodes[i].name,       rec.name,       16);
        memcpy(nodes[i].fw_version, rec.fw_version, 8);
        nodes[i].lastSeen      = millis();  // avoid instant NODE_TIMEOUT
        nodes[i].online        = false;     // marked offline until first message
        nodes[i].actuatorMask = 0;
        nodes[i].settingsCount = 0;         // re-fetched when node responds

        addPeer(rec.mac);  // re-register ESP-NOW peer
        restored++;
        Serial.printf("[MESH] Restored node #%d \"%s\"  %s\n",
                      i, nodes[i].name, macToStr(nodes[i].mac).c_str());
    }
    nextId = savedNextId;
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
        sendSettingsGet(nodes[i].mac, i, nodes[i].type);
        sendSensorSchemaGet(nodes[i].mac, i, nodes[i].type);
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

    MsgActuatorSet cmd;

    cmd.hdr.type      = MSG_ACTUATOR_SET;
    cmd.hdr.node_id   = nodeId;
    cmd.hdr.node_type = NODE_ACTUATOR;

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
            strncpy(discovered[i].name, name, 15);
            discovered[i].name[15]  = '\0';
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

            if (nodes[i].type == NODE_SENSOR) {
                // sensor_schema_ready mirrors sensorCount > 0, used by the JS client
                // to decide whether to request the schema via node_sensor_schema_get.
                n["sensor_schema_ready"] = (nodes[i].sensorCount > 0);
                JsonArray rdgs = n["sensor_readings"].to<JsonArray>();
                for (uint8_t j = 0; j < nodes[i].sensorCount; j++) {
                    JsonObject r = rdgs.add<JsonObject>();
                    r["id"]    = nodes[i].sensorSchema[j].id;
                    r["value"] = nodes[i].sensorValues[j];
                }
            } else {
                n["actuator_mask"] = nodes[i].actuatorMask;
                n["relay_mask"]    = nodes[i].actuatorMask;  // backward-compatible alias for older UI code
                JsonArray labels = n["relay_labels"].to<JsonArray>();
                for (uint8_t j = 0; j < NODE_MAX_ACTUATORS; j++) {
                    labels.add(nodes[i].relayLabels[j][0]
                        ? nodes[i].relayLabels[j]
                        : String("Relay ") + String(j + 1));
                }
            }
            n["settings_ready"] = (nodes[i].settingsCount > 0);
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
    doc["gw_led_enabled"] = gwLedEnabled;
    doc["ap_ssid"]         = gwApSsid;
    doc["credentials_set"] = credentialsSet();
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
                if (pkt.len < (int)sizeof(MsgRegister)) break;
                auto* reg = (MsgRegister*)pkt.data;

                uint8_t assignId = findNodeByMac(pkt.mac);
                bool    isNew    = (assignId == 0);
                if (isNew) {
                    assignId = findFreeSlot();
                    if (assignId == 0) {
                        Serial.println("[MESH] Max nodes reached");
                        break;
                    }
                }

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    memcpy(nodes[assignId].mac, pkt.mac, 6);
                    nodes[assignId].type     = hdr->node_type;
                    memcpy(nodes[assignId].name, reg->name, 15);
                    nodes[assignId].name[15] = '\0';
                    strncpy(nodes[assignId].fw_version, reg->fw_version, 7);
                    nodes[assignId].fw_version[7] = '\0';
                    nodes[assignId].lastSeen = millis();
                    nodes[assignId].online   = true;
                    if (isNew) nodes[assignId].settingsCount = 0;  // clear stale schema on fresh slot
                    xSemaphoreGive(nodesMutex);
                }

                if (isNew) {
                    loadRelayLabelsForNode(assignId);
                }

                Serial.printf("[MESH] %s node #%d \"%s\"  %s\n",
                              isNew ? "New" : "Re-reg",
                              assignId, reg->name, macToStr(pkt.mac).c_str());

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
                // Request both the settings schema and the sensor schema immediately
                // after ACK so the dashboard and settings panel populate as soon as
                // the node replies.
                sendSettingsGet(pkt.mac, assignId, hdr->node_type);
                sendSensorSchemaGet(pkt.mac, assignId, hdr->node_type);
                // Persist the registry so nodes survive a gateway reboot
                if (isNew) saveNodesToNvs();
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
                    nodes[id].actuatorMask = mask;
                    nodes[id].lastSeen = millis();
                    nodes[id].online   = true;
                    xSemaphoreGive(nodesMutex);
                }

                wsBroadcast(buildNodesJson());

                Serial.printf("[MESH] Node #%d actuator mask: 0x%02X\n", id, mask);

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
                        if (state) nodes[nodeId].actuatorMask |= (1u << actuatorId);
                        else        nodes[nodeId].actuatorMask &= ~(1u << actuatorId);
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
                if (strlen(newName) > 0 && strlen(newName) < 16) {
                    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        strncpy(nodes[nodeId].name, newName, 15);
                        nodes[nodeId].name[15] = '\0';
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
                if (nodes[nodeId].type != NODE_ACTUATOR) {
                    client->text(buildRelayLabelsAckJson(nodeId, false, "Relay labels only apply to actuator nodes"));
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
          .setCacheControl("max-age=3600");

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
    updateGwLed();

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
