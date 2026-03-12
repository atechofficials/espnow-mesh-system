/**
 * ╔══════════════════════════════════════════════════════════════════════════╗
 * ║        ESP32 MESH GATEWAY  —  Master Device (ESP32-S3 N8R8)             ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  Static web assets (HTML / CSS / JS) are served from LittleFS.          ║
 * ║  Flash the filesystem ONCE with:  pio run --target uploadfs              ║
 * ║  Then flash firmware normally:    pio run --target upload                ║
 * ║                                                                          ║
 * ║  First boot  → opens AP "ESP32-Mesh-Setup" (captive portal).            ║
 * ║  Hold BOOT (GPIO0) at reset to wipe saved WiFi credentials.             ║
 * ╚══════════════════════════════════════════════════════════════════════════╝
 */
#define FW_VERSION "1.7.1"

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

// ─── Configuration ────────────────────────────────────────────────────────────
#define RESET_BTN_PIN    0
#define AP_SSID_DEFAULT  "ESP32-Mesh-Setup"
#define AP_PASS_DEFAULT  "meshsetup"
#define WEB_PORT         80

// Runtime AP credentials — loaded from NVS on boot, fall back to compile-time defaults.
// These are the SSID / password the WiFiManager captive portal AP will use.
static char gwApSsid[33]     = AP_SSID_DEFAULT;
static char gwApPassword[64] = AP_PASS_DEFAULT;

// ─── Web Interface Auth ────────────────────────────────────────────────────────
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

// ─── Node Registry ────────────────────────────────────────────────────────────
struct NodeRecord {
    uint8_t       mac[6];
    NodeType      type;
    char          name[16];
    char          fw_version[8];  // reported by node in MSG_REGISTER
    unsigned long lastSeen;
    bool          online;
    float         temperature;
    float         pressure;
    uint32_t      uptime;
    uint8_t       relayMask;
    // ── Per-node dynamic settings schema (populated by MSG_SETTINGS_DATA) ──────
    uint8_t       settingsCount;                     // 0 = not yet received
    SettingDef    settings[NODE_MAX_SETTINGS];        // schema + current values
};
static NodeRecord nodes[MESH_MAX_NODES + 1];
static uint8_t    nextId = 1;

// ─── Discovered (beaconing) nodes — not yet paired ───────────────────────────
struct DiscoveredNode {
    uint8_t       mac[6];
    char          name[16];
    NodeType      type;
    uint8_t       tx_channel;  // channel from last beacon
    unsigned long lastSeen;
    bool          active;
};
static DiscoveredNode discovered[MAX_DISCOVERED];

// ─── Pending pair — gateway retries PAIR_CMD until MSG_REGISTER arrives ──────
struct PendingPair {
    uint8_t       mac[6];
    bool          active;
    unsigned long startedAt;
    unsigned long lastAttempt;
};
static PendingPair pendingPair = {};

// ─── RTOS ─────────────────────────────────────────────────────────────────────
static SemaphoreHandle_t nodesMutex;

struct RxPacket { uint8_t mac[6]; uint8_t data[250]; int len; };
static QueueHandle_t rxQueue;

// ─── Web Server ───────────────────────────────────────────────────────────────
static AsyncWebServer server(WEB_PORT);
static AsyncWebSocket ws("/ws");

// ─── State ────────────────────────────────────────────────────────────────────
static uint8_t   wifiChannel = 1;
static uint32_t  bootMs      = 0;

// ─── Gateway LED ──────────────────────────────────────────────────────────────
static Adafruit_NeoPixel gwLed(1, GW_LED_PIN, NEO_GRB + NEO_KHZ800);
static uint32_t          gwLedCurrent   = 0xDEADBEEF;
static unsigned long     gwLedFlashUntil = 0;

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
    if (millis() < gwLedFlashUntil) return;
    setGwLed(gwLed.Color(0, 0, 32));  // dim blue = operational
}

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
//  Web Auth helpers
// ─────────────────────────────────────────────────────────────────────────────
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
    return String();
}

// Returns true if the HTTP request carries a valid session or remember cookie.
static bool isHttpAuthenticated(AsyncWebServerRequest* req) {
    if (!credentialsSet()) return true;
    if (strlen(sessionToken) > 0) {
        String v = getCookieValue(req, "gwsession");
        if (v.length() > 0 && v.equals(sessionToken)) return true;
    }
    if (strlen(rememberToken) > 0) {
        String v = getCookieValue(req, "gwremember");
        if (v.length() > 0 && v.equals(rememberToken)) return true;
    }
    return false;
}

// Broadcast a message only to authenticated WS clients
// (or to all clients when no credentials are configured).
static void wsBroadcast(const String& msg) {
    if (!credentialsSet()) { ws.textAll(msg); return; }
    for (uint32_t id : authWsClients) {
        AsyncWebSocketClient* c = ws.client(id);
        if (c && c->canSend()) c->text(msg);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  NVS — Gateway config (AP name / password)
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
//  NVS — Paired node registry
//  Only the static identity fields are persisted; volatile runtime fields
//  (lastSeen, online, temperature, pressure, uptime, relay, settings)
//  are repopulated naturally when the node sends its first message.
// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations needed by loadNodesFromNvs (defined further below)
static bool addPeer(const uint8_t* mac, uint8_t channel = 0);
static void sendSettingsGet(const uint8_t* mac, uint8_t nodeId, NodeType nodeType);
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
            prefs.remove(key);  // slot was freed — remove stale entry
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

    // Proactively request settings from every restored node so the settings
    // panel repopulates as soon as each node replies.
    for (uint8_t i = 1; i < nextId; i++) {
        if (nodes[i].mac[0] == 0 && nodes[i].mac[1] == 0) continue;
        sendSettingsGet(nodes[i].mac, i, nodes[i].type);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  LittleFS
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
//  ESP-NOW helpers
// ─────────────────────────────────────────────────────────────────────────────
static bool addPeer(const uint8_t* mac, uint8_t channel) {
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t p{};
    memcpy(p.peer_addr, mac, 6);
    p.channel = channel;
    p.encrypt = false;
    return esp_now_add_peer(&p) == ESP_OK;
}

static void sendRegisterAck(const uint8_t* mac, uint8_t assignedId, NodeType nodeType) {
    MsgRegisterAck ack;
    ack.hdr.type      = MSG_REGISTER_ACK;
    ack.hdr.node_id   = assignedId;
    ack.hdr.node_type = nodeType;
    ack.assigned_id   = assignedId;
    ack.channel       = wifiChannel;
    esp_now_send(mac, (uint8_t*)&ack, sizeof(ack));
}

static void sendPairCmd(const uint8_t* mac) {
    MsgPairCmd cmd;
    cmd.hdr.type      = MSG_PAIR_CMD;
    cmd.hdr.node_id   = 0;
    cmd.hdr.node_type = NODE_SENSOR;
    cmd.channel       = wifiChannel;
    esp_now_send(mac, (uint8_t*)&cmd, sizeof(cmd));
}

static void sendUnpairCmd(const uint8_t* mac, uint8_t nodeId) {
    MsgUnpairCmd cmd;
    cmd.hdr.type      = MSG_UNPAIR_CMD;
    cmd.hdr.node_id   = nodeId;
    cmd.hdr.node_type = NODE_SENSOR;
    esp_now_send(mac, (uint8_t*)&cmd, sizeof(cmd));
}

static void sendRebootCmd(const uint8_t* mac, uint8_t nodeId) {
    MsgRebootCmd cmd;
    cmd.hdr.type      = MSG_REBOOT_CMD;
    cmd.hdr.node_id   = nodeId;
    cmd.hdr.node_type = NODE_SENSOR;  // node ignores this field on reboot
    esp_now_send(mac, (uint8_t*)&cmd, sizeof(cmd));
}

static void sendSettingsGet(const uint8_t* mac, uint8_t nodeId, NodeType nodeType) {
    MsgSettingsGet msg;
    msg.hdr.type      = MSG_SETTINGS_GET;
    msg.hdr.node_id   = nodeId;
    msg.hdr.node_type = nodeType;
    esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

static void sendSettingsSet(const uint8_t* mac, uint8_t nodeId, NodeType nodeType,
                            uint8_t settingId, int16_t value) {
    MsgSettingsSet msg;
    msg.hdr.type      = MSG_SETTINGS_SET;
    msg.hdr.node_id   = nodeId;
    msg.hdr.node_type = nodeType;
    msg.id            = settingId;
    msg.value         = value;
    esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

static bool sendRelayCmd(uint8_t nodeId, uint8_t relayIndex, uint8_t state) {
    if (nodeId == 0 || nodeId >= nextId) return false;
    if (!nodes[nodeId].online)           return false;
    MsgRelayCmd cmd;
    cmd.hdr.type      = MSG_RELAY_CMD;
    cmd.hdr.node_id   = nodeId;
    cmd.hdr.node_type = NODE_RELAY;
    cmd.relay_index   = relayIndex;
    cmd.state         = state;
    return esp_now_send(nodes[nodeId].mac, (uint8_t*)&cmd, sizeof(cmd)) == ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ESP-NOW callbacks
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
//  Discovered list management
// ─────────────────────────────────────────────────────────────────────────────
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

    // New entry — find a free or expired slot
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

// ─────────────────────────────────────────────────────────────────────────────
//  Disconnect a paired node
// ─────────────────────────────────────────────────────────────────────────────
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
    flashGwLed(gwLed.Color(255, 80, 0), 200);  // orange pulse
    Serial.printf("[MESH] Node #%d disconnected\n", nodeId);
    saveNodesToNvs();  // persist the freed slot so it doesn't reappear after reboot
    wsBroadcast(buildNodesJson());  // forward declaration — defined below
}

// ─────────────────────────────────────────────────────────────────────────────
//  JSON builders
// ─────────────────────────────────────────────────────────────────────────────
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
                n["temperature"] = nodes[i].temperature;
                n["pressure"]    = nodes[i].pressure;
            } else {
                n["relay_mask"] = nodes[i].relayMask;
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
    doc["ap_ssid"]         = gwApSsid;
    doc["credentials_set"] = credentialsSet();
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

// ─────────────────────────────────────────────────────────────────────────────
//  Process RX queue — called from loop()
// ─────────────────────────────────────────────────────────────────────────────
static void processRxQueue() {
    RxPacket pkt;
    while (xQueueReceive(rxQueue, &pkt, 0) == pdTRUE) {
        if (pkt.len < (int)sizeof(MeshHeader)) continue;
        auto* hdr = (MeshHeader*)pkt.data;

        switch (hdr->type) {

            // ── Beacon from node in pairing mode ──────────────────────────
            case MSG_BEACON: {
                if (pkt.len < (int)sizeof(MsgBeacon)) break;
                auto* b = (MsgBeacon*)pkt.data;

                // Ignore if this node is already paired
                if (findNodeByMac(pkt.mac) != 0) break;

                bool isNew = updateDiscovered(pkt.mac, b->name,
                                              b->hdr.node_type, b->tx_channel);
                if (isNew) {
                    flashGwLed(gwLed.Color(255, 255, 255), 150);  // white pulse
                    wsBroadcast(buildDiscoveredJson());
                }
                break;
            }

            // ── Registration (paired reboot reconnect OR post-PAIR_CMD confirm)
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

                Serial.printf("[MESH] %s node #%d \"%s\"  %s\n",
                              isNew ? "New" : "Re-reg",
                              assignId, reg->name, macToStr(pkt.mac).c_str());

                // Cancel pending pair if this is the expected MAC
                if (pendingPair.active &&
                    memcmp(pendingPair.mac, pkt.mac, 6) == 0) {
                    pendingPair.active = false;
                    Serial.println("[PAIR]  Pair confirmed.");
                    flashGwLed(gwLed.Color(0, 255, 0), 300);  // green pulse
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
                // Request settings schema from this node immediately after ACK
                sendSettingsGet(pkt.mac, assignId, hdr->node_type);
                // Persist the registry so nodes survive a gateway reboot
                if (isNew) saveNodesToNvs();
                wsBroadcast(buildNodesJson());
                wsBroadcast(buildDiscoveredJson());
                continue;
            }

            case MSG_SENSOR_DATA: {
                if (pkt.len < (int)sizeof(MsgSensorData)) break;
                auto* sd  = (MsgSensorData*)pkt.data;
                uint8_t id = hdr->node_id;
                if (id == 0 || id >= nextId) break;
                if (nodes[id].mac[0] == 0) break;  // slot was recycled

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    nodes[id].temperature = sd->temperature;
                    nodes[id].pressure    = sd->pressure;
                    nodes[id].uptime      = sd->uptime_sec;
                    nodes[id].lastSeen    = millis();
                    nodes[id].online      = true;
                    xSemaphoreGive(nodesMutex);
                }
                Serial.printf("[MESH] Node #%d  T=%.1f°C  P=%.1fhPa\n",
                              id, sd->temperature, sd->pressure);
                break;
            }

            case MSG_RELAY_STATE: {
                if (pkt.len < (int)sizeof(MsgRelayState)) break;
                auto* rs  = (MsgRelayState*)pkt.data;
                uint8_t id = hdr->node_id;
                if (id == 0 || id >= nextId) break;

                if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    nodes[id].relayMask = rs->relay_mask;
                    nodes[id].lastSeen  = millis();
                    nodes[id].online    = true;
                    xSemaphoreGive(nodesMutex);
                }
                wsBroadcast(buildNodesJson());
                Serial.printf("[MESH] Node #%d relay mask: 0x%02X\n", id, rs->relay_mask);
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

            // ── Node-initiated disconnect (button held 5 s on node) ───────
            case MSG_UNPAIR_CMD: {
                uint8_t id = findNodeByMac(pkt.mac);
                if (id == 0) break;
                Serial.printf("[PAIR]  Node #%d self-disconnected\n", id);
                disconnectNode(id);
                break;
            }

            // ── Settings schema received from node ────────────────────────
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
                flashGwLed(gwLed.Color(0, 0, 255), 100);  // brief blue = config rx
                // Push updated settings to authenticated WS clients
                wsBroadcast(buildNodeSettingsJson(id));
                // Also push nodes update so settings_ready flag refreshes in the table
                wsBroadcast(buildNodesJson());
                break;
            }

            default: break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  WebSocket event handler
// ─────────────────────────────────────────────────────────────────────────────
static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg,
                      uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("[WS]  Client #%u  %s\n",
                          client->id(), client->remoteIP().toString().c_str());
            if (!credentialsSet()) {
                // Open access — auto-authenticate and send initial data
                authWsClients.insert(client->id());
                client->text(buildMetaJson());
                client->text(buildNodesJson());
                client->text(buildDiscoveredJson());
            } else {
                // Credentials are set — client must authenticate first
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

            // ── Auth message — must be processed before the auth guard ────
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

            // ── Guard: reject unauthenticated commands ────────────────────
            if (!isWsAuthenticated(client->id())) {
                client->text("{\"type\":\"auth_required\"}");
                break;
            }

            // ── Relay command ─────────────────────────────────────────────
            if (strcmp(msgType, "relay_cmd") == 0) {
                uint8_t nodeId     = doc["node_id"]     | 0;
                uint8_t relayIndex = doc["relay_index"] | 0;
                uint8_t state      = doc["state"]       | 0;
                bool ok = sendRelayCmd(nodeId, relayIndex, state);
                if (ok) {
                    if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        if (state) nodes[nodeId].relayMask |=  (1u << relayIndex);
                        else        nodes[nodeId].relayMask &= ~(1u << relayIndex);
                        xSemaphoreGive(nodesMutex);
                    }
                    wsBroadcast(buildNodesJson());
                }

            // ── Pair command (user clicked "Connect") ─────────────────────
            } else if (strcmp(msgType, "pair_cmd") == 0) {
                const char* macStr = doc["mac"] | "";
                uint8_t mac[6];
                if (!parseMac(macStr, mac)) break;

                // Already paired?
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

            // ── Rename node ───────────────────────────────────────────────
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

            // ── Unpair command (user clicked "Disconnect") ────────────────
            } else if (strcmp(msgType, "unpair_cmd") == 0) {
                uint8_t nodeId = doc["node_id"] | 0;
                if (nodeId == 0 || nodeId >= nextId) break;
                if (nodes[nodeId].mac[0] == 0) break;

                sendUnpairCmd(nodes[nodeId].mac, nodeId);
                delay(80);  // brief wait for send before removing peer
                disconnectNode(nodeId);
            }
            // ── Reboot a specific node ────────────────────────────────────
            else if (strcmp(msgType, "reboot_node") == 0) {
                uint8_t nodeId = doc["node_id"] | 0;
                if (nodeId == 0 || nodeId >= nextId) break;
                if (nodes[nodeId].mac[0] == 0) break;
                if (!nodes[nodeId].online) break;  // pointless to send if offline

                sendRebootCmd(nodes[nodeId].mac, nodeId);
                Serial.printf("[MESH] Reboot command sent to node #%d\n", nodeId);
                flashGwLed(gwLed.Color(255, 165, 0), 200);  // orange pulse

            // ── Reboot the gateway itself ─────────────────────────────────
            } else if (strcmp(msgType, "reboot_gw") == 0) {
                Serial.println("[GW]  Reboot requested via dashboard.");
                ws.textAll("{\"type\":\"gw_rebooting\"}");  // notify all clients
                ws.cleanupClients();
                delay(150);  // allow WS frame to flush before restart
                ESP.restart();

            // ── Update AP SSID / password ─────────────────────────────────
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

            // ── Set web interface login credentials ───────────────────────
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

            // ── Trigger WiFiManager portal (change home-router network) ───
            } else if (strcmp(msgType, "start_wifi_portal") == 0) {
                Serial.println("[GW]  WiFi portal requested via dashboard.");
                ws.textAll("{\"type\":\"gw_portal_starting\"}");
                ws.cleanupClients();
                delay(200);
                {
                    WiFiManager wm;
                    wm.resetSettings();  // wipe saved STA credentials → portal opens on boot
                }
                delay(100);
                ESP.restart();

            // ── Node settings: request current schema from node ────────────
            } else if (strcmp(msgType, "node_settings_get") == 0) {
                uint8_t nodeId = doc["node_id"] | 0;
                if (nodeId == 0 || nodeId >= nextId) break;
                if (nodes[nodeId].mac[0] == 0) break;

                if (nodes[nodeId].settingsCount > 0) {
                    // Already have schema — respond immediately to this client only
                    client->text(buildNodeSettingsJson(nodeId));
                } else {
                    // Re-request from node
                    if (nodes[nodeId].online)
                        sendSettingsGet(nodes[nodeId].mac, nodeId, nodes[nodeId].type);
                    // Respond with empty settings until node replies
                    client->text(buildNodeSettingsJson(nodeId));
                }

            // ── Node settings: apply a single setting change ──────────────
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
                Serial.printf("[CFG]  Node #%d setting %d → %d\n", nodeId, settingId, value);
                wsBroadcast(buildNodeSettingsJson(nodeId));

            // ── Factory reset ─────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
//  Web server routes
// ─────────────────────────────────────────────────────────────────────────────
static void setupRoutes() {
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // Static files served without auth — the JS login overlay handles access control.
    server.serveStatic("/", LittleFS, "/")
          .setDefaultFile("index.html")
          .setCacheControl("max-age=3600");

    // ── /api/auth_check ───────────────────────────────────────────────────────
    server.on("/api/auth_check", HTTP_GET, [](AsyncWebServerRequest* req) {
        bool authed   = isHttpAuthenticated(req);
        bool credsSet = credentialsSet();
        String body = String("{\"authenticated\":") + (authed   ? "true" : "false") +
                      ",\"credentials_set\":"        + (credsSet ? "true" : "false") + "}";
        req->send(200, "application/json", body);
    });

    // ── /api/login ────────────────────────────────────────────────────────────
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
                // No credentials configured — open access
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

            // Build JSON — always return session token for WS auth; include
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

    // ── /api/logout ───────────────────────────────────────────────────────────
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

    // ── Protected API routes ──────────────────────────────────────────────────
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
    server.on("/api/relay", HTTP_POST,
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
            bool ok = sendRelayCmd(
                (uint8_t)(doc["node_id"]     | 0),
                (uint8_t)(doc["relay_index"] | 0),
                (uint8_t)(doc["state"]       | 0));
            req->send(200, "application/json", ok ? R"({"ok":true})" : R"({"ok":false})");
        }
    );
    server.onNotFound([](AsyncWebServerRequest* req) {
        Serial.printf("[HTTP] 404: %s\n", req->url().c_str());
        req->send(404, "text/plain", "Not found");
    });
}

// ═════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n\n[BOOT] ESP32 Mesh Gateway");
    bootMs = millis();

    // ── Gateway LED ───────────────────────────────────────────────────────────
    gwLed.begin();
    gwLed.setBrightness(60);
    setGwLed(gwLed.Color(255, 255, 255));  // white at boot
    delay(300);

    // ── RTOS objects ──────────────────────────────────────────────────────────
    nodesMutex = xSemaphoreCreateMutex();
    rxQueue    = xQueueCreate(RX_QUEUE_SIZE, sizeof(RxPacket));
    memset(nodes,      0, sizeof(nodes));
    memset(discovered, 0, sizeof(discovered));

    // ── LittleFS ──────────────────────────────────────────────────────────────
    mountFilesystem();

    // ── WiFi credentials reset ────────────────────────────────────────────────
    pinMode(RESET_BTN_PIN, INPUT_PULLUP);
    if (digitalRead(RESET_BTN_PIN) == LOW) {
        Serial.println("[WiFi] Clearing credentials…");
        WiFiManager wm;
        wm.resetSettings();
        delay(500);
    }

    // ── Load gateway config (AP name / password) from NVS ─────────────────────
    loadApConfig();

    // ── Load web interface credentials from NVS ───────────────────────────────
    loadWebCredentials();

    // ── WiFiManager ───────────────────────────────────────────────────────────
    {
        WiFiManager wm;
        wm.setTitle("ESP32 Mesh Gateway Setup");
        wm.setConfigPortalTimeout(180);
        wm.setHttpPort(8080);  // use port 8080 so WiFiManager never conflicts with AsyncWebServer on port 80
        Serial.println("[WiFi] Connecting (portal if needed)…");
        if (!wm.autoConnect(gwApSsid, gwApPassword)) {
            Serial.println("[WiFi] Failed — rebooting");
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

    // ── ESP-NOW ───────────────────────────────────────────────────────────────
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] Init failed — rebooting");
        delay(1000);
        ESP.restart();
    }
    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onDataSent);

    // Add broadcast peer so we can receive beacons
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    addPeer(broadcast);

    // Restore paired nodes from NVS — must happen after ESP-NOW init so
    // addPeer() calls inside loadNodesFromNvs() succeed.
    loadNodesFromNvs();

    Serial.printf("[ESP-NOW] Ready — MAC: %s  Ch: %d\n",
                  WiFi.macAddress().c_str(), wifiChannel);

    // ── Web server ────────────────────────────────────────────────────────────
    setupRoutes();
    server.begin();
    Serial.printf("[HTTP]  Dashboard → http://%s/\n", WiFi.localIP().toString().c_str());

    setGwLed(gwLed.Color(0, 0, 32));  // settle to dim blue
    Serial.println("[BOOT] Setup complete.\n");
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════════════════════
static unsigned long lastNodeBcast = 0;
static unsigned long lastMetaBcast = 0;
static unsigned long lastDiscClean = 0;
static unsigned long lastDiscBcast = 0;

void loop() {
    unsigned long now = millis();

    processRxQueue();
    updateGwLed();

    // ── Pending pair: retry PAIR_CMD until MSG_REGISTER arrives ──────────────
    if (pendingPair.active) {
        if (now - pendingPair.startedAt > PAIR_CMD_TIMEOUT_MS) {
            pendingPair.active = false;
            Serial.println("[PAIR]  Timed out — no response from node.");
            wsBroadcast("{\"type\":\"pair_timeout\"}");
        } else if (now - pendingPair.lastAttempt >= PAIR_CMD_RETRY_MS) {
            pendingPair.lastAttempt = now;
            sendPairCmd(pendingPair.mac);
            Serial.printf("[PAIR]  Sending PAIR_CMD → %02X:%02X:%02X:%02X:%02X:%02X\n",
                          pendingPair.mac[0], pendingPair.mac[1], pendingPair.mac[2],
                          pendingPair.mac[3], pendingPair.mac[4], pendingPair.mac[5]);
        }
    }

    // ── Periodic WS pushes ────────────────────────────────────────────────────
    if (now - lastNodeBcast >= WS_UPDATE_MS) {
        lastNodeBcast = now;
        if (ws.count() > 0) wsBroadcast(buildNodesJson());
    }
    if (now - lastMetaBcast >= WS_META_MS) {
        lastMetaBcast = now;
        if (ws.count() > 0) wsBroadcast(buildMetaJson());
    }

    // ── Periodic discovered list push (keeps clients in sync) ────────────────
    if (now - lastDiscBcast >= WS_UPDATE_MS) {
        lastDiscBcast = now;
        if (ws.count() > 0) wsBroadcast(buildDiscoveredJson());
    }

    // ── Discovered list cleanup ───────────────────────────────────────────────
    if (now - lastDiscClean >= 2000) {
        lastDiscClean = now;
        if (cleanupDiscovered() && ws.count() > 0)
            wsBroadcast(buildDiscoveredJson());
    }

    ws.cleanupClients();
    delay(10);
}