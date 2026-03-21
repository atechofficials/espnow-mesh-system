/**
    * @file [main.cpp]
    * @brief Main source file for the ESP32 Mesh Actuator Node firmware
    * @details ESP-NOW based mesh relay node for controlling 4 relays and reporting button state, with WS2812B LED status indicator.
    *        Designed for use with a central gateway node that manages pairing and communication.
    * 
    *        Features:
    *          - C++20 codebase for modern programming features and improved readability.
    *          - Uses Preferences library for non-volatile storage of pairing information.
    *          - Implements a simple state machine to manage node states (unpaired, pairing, paired, etc.).
    *          - Controls 4 relays via GPIO pins and reports their state back to the gateway.
    *          - Monitors a pairing button with debounce logic and long-press detection for entering pairing mode.
    *          - Uses a WS2812B LED to indicate node status (e.g., unpaired, paired, error states).
    *          - Implements a simple touch input handling for toggling relays, with debounce logic.
    * 
    *        Note: This code is intended as a starting point and may require adjustments based on specific hardware configurations and requirements.
    * @version 1.2.0
    * @author Mrinal (@atechofficials)
 */
#define FW_VERSION "1.2.0"
#define HW_CONFIG_ID "0x1A"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "mesh_protocol.h"

// User config
#define NODE_NAME       "Relay-Node-1"   // change per node (max 15 chars)
#define RELAY1_PIN      26    // Active LOW relay control pins - adjust as needed for your hardware setup
#define RELAY2_PIN      27
#define RELAY3_PIN      32
#define RELAY4_PIN      33
#define TOUCH1_PIN      25    // TTP224 touch sensor 1 (channels 1-4) - active HIGH, with external pull-downs to GND. Adjust pin numbers as needed for your hardware setup.
#define TOUCH2_PIN      4     // TTP224 touch sensor 2
#define TOUCH3_PIN      13    // TTP224 touch sensor 3
#define TOUCH4_PIN      14    // TTP224 touch sensor 4
#define PAIR_BTN_PIN    16    // Pairing button GPIO pin (active-LOW, uses internal pull-up)
#define LED_PIN         5     // WS2812B data pin
#define LED_COUNT       1     
#define RELAY_COUNT     4
#define TOUCH_DEBOUNCE_MS 35
bool relay_active_high = false; // Set to true if your relay module is active HIGH, false if active LOW

// Node state machine
enum NodeState { STATE_UNPAIRED, STATE_PAIRING, STATE_PAIRED, STATE_DISC_PEND, STATE_GW_LOST };
static NodeState nodeState = STATE_UNPAIRED;

// Globals
static uint8_t  masterMac[6] = {0};
static uint8_t  myNodeId     = 0;
static uint8_t  myChannel    = 0;
static bool     hasMaster    = false;
static bool     masterAcked  = false;
static unsigned long lastReReg = 0;

static bool relayState[RELAY_COUNT] = {0,0,0,0};
static uint8_t lastRelayState[RELAY_COUNT] = {255,255,255,255};
static const uint8_t relayPins[RELAY_COUNT] = {RELAY1_PIN, RELAY2_PIN, RELAY3_PIN, RELAY4_PIN};
static const uint8_t touchPins[RELAY_COUNT] = {TOUCH1_PIN, TOUCH2_PIN, TOUCH3_PIN, TOUCH4_PIN};

static Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
static Preferences       prefs;

// LED state
static uint32_t     ledCurrent    = 0xDEADBEEF;
static unsigned long ledFlashUntil = 0;

// Button state
static bool          btnWasDown      = false;
static unsigned long btnPressedAt    = 0;
static bool          phase2Shown     = false;

// Pairing state
static unsigned long pairingStarted  = 0;
static uint8_t       pairingChannel  = 1;
static unsigned long lastBeacon      = 0;

// Timing state
static unsigned long lastHeartbeat   = 0;
static bool          touchRawState[RELAY_COUNT] = {0,0,0,0};
static bool          touchStableState[RELAY_COUNT] = {0,0,0,0};
static unsigned long touchChangedAt[RELAY_COUNT] = {0,0,0,0};

// Node Settings
static char relayLabel[RELAY_COUNT][16] =
{
  "Relay 1",
  "Relay 2",
  "Relay 3",
  "Relay 4"
};
static const char kNodeFirmwareRoleMarker[] = "NODETYPE:ACTUATOR";
static const char kNodeFirmwareVersionMarker[] = "NODEFWVER:" FW_VERSION;
static const char kNodeHardwareConfigMarker[] = "NODEHWCFG:" HW_CONFIG_ID;
static volatile uint32_t gNodeFirmwareMarkerChecksum = 0;
#define SETTING_ID_RELAY_PERSIST 0
#define SETTING_ID_LED_EN 1

static bool sRelayPersist = false;
static bool sSettingLedEn = true;

// Gateway-loss detection 
#define GW_LOST_THRESHOLD   3
static volatile uint8_t txFailCount = 0;

struct PendingNodeOta {
    bool     pending = false;
    uint32_t sessionId = 0;
    uint32_t imageSize = 0;
    uint32_t imageCrc32 = 0;
    uint16_t port = 80;
    char     ssid[NODE_OTA_SSID_LEN] = {0};
    char     password[NODE_OTA_PASS_LEN] = {0};
    char     version[NODE_OTA_VERSION_LEN] = {0};
};

static PendingNodeOta pendingOta;
static bool otaRunning = false;

static void touchFirmwareMarkers() {
    uint32_t sum = 0;
    for (size_t i = 0; kNodeFirmwareRoleMarker[i] != '\0'; i++) sum += (uint8_t)kNodeFirmwareRoleMarker[i];
    for (size_t i = 0; kNodeFirmwareVersionMarker[i] != '\0'; i++) sum += (uint8_t)kNodeFirmwareVersionMarker[i];
    for (size_t i = 0; kNodeHardwareConfigMarker[i] != '\0'; i++) sum += (uint8_t)kNodeHardwareConfigMarker[i];
    gNodeFirmwareMarkerChecksum = sum;
}

// RX queue
struct RxPacket { uint8_t mac[6]; uint8_t data[250]; int len; };
static QueueHandle_t rxQueue;
static void sendActuatorState();

// *****************************************************************************
//  LED helpers
// *****************************************************************************
static void setLed(uint32_t color) {
    if (color == ledCurrent) return;
    ledCurrent = color;
    led.setPixelColor(0, color);
    led.show();
}

static void flashLed(uint32_t durationMs = 120) {
    ledFlashUntil = millis() + durationMs;
    setLed(led.Color(0, 255, 0));
    ledCurrent = 0xDEADBEEF;
}

static void updateLed() {
    unsigned long now = millis();

    if (!sSettingLedEn) {
        if (nodeState == STATE_PAIRING || nodeState == STATE_DISC_PEND) {
            // fall through
        } else {
            setLed(led.Color(0, 0, 0));
            return;
        }
    }

    if (now < ledFlashUntil) {
        uint32_t c = led.Color(0, 255, 0);
        if (ledCurrent != c) { ledCurrent = c; led.setPixelColor(0, c); led.show(); }
        return;
    }

    switch (nodeState) {
        case STATE_UNPAIRED:
            setLed(led.Color(255, 0, 0));
            break;
        case STATE_PAIRING: {
            bool on = ((now / 500) % 2) == 0;
            setLed(on ? led.Color(255, 255, 255) : led.Color(0, 0, 0));
            break;
        }
        case STATE_PAIRED:
            setLed(led.Color(0, 48, 0));
            break;
        case STATE_DISC_PEND: {
            bool on = ((now / 250) % 2) == 0;
            setLed(on ? led.Color(0, 0, 255) : led.Color(0, 0, 0));
            break;
        }
        case STATE_GW_LOST: {
            bool on = ((now / 900) % 2) == 0;
            setLed(on ? led.Color(255, 140, 0) : led.Color(0, 0, 0));
            break;
        }
    }
}
// *****************************************************************************
//  Relay helpers
// *****************************************************************************
// Saves the state of a relay to NVS for persistence across reboots.
static void buildRelayStateKey(uint8_t id, char* key, size_t keyLen)
{
    snprintf(key, keyLen, "r%u", (unsigned)id);
}

static void saveRelayState(uint8_t id, bool state)
{
    char key[8];
    buildRelayStateKey(id, key, sizeof(key));
    prefs.begin("relay", false);
    prefs.putBool(key, state);
    prefs.end();
    // For Debugging: print saved relay state info to Serial Monitor
    Serial.printf("[NVS]  Saved relay %d state: %s\n", id, state ? "ON" : "OFF");
}

void setRelay(uint8_t id, bool state, bool persistState = true)
{
    if(id >= RELAY_COUNT) return;

    relayState[id] = state;
    const uint8_t pin = relayPins[id];
    
    if (relay_active_high) {
        digitalWrite(pin, state ? HIGH : LOW); // active HIGH relay
    } 
    else {
        digitalWrite(pin, state ? LOW : HIGH); // active LOW relay
    }

    if (persistState && sRelayPersist) {
        // For Debugging: print relay state persistence info to Serial Monitor
        Serial.printf("[RELAY STATE]  Relay State Peristence Enabled - Saving state of Relay %d: %s\n", id, state ? "ON" : "OFF");
        saveRelayState(id, state);
    }
    // For Debugging: print relay output change info to Serial Monitor
    Serial.printf("[RELAY STATE]  Set Relay %d to %s\n", id, state ? "ON" : "OFF");
}

static void handleTouchInputs() {
    const unsigned long now = millis();

    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        const bool rawActive = (digitalRead(touchPins[i]) == HIGH);

        if (rawActive != touchRawState[i]) {
            touchRawState[i] = rawActive;
            touchChangedAt[i] = now;
        }

        if ((now - touchChangedAt[i]) < TOUCH_DEBOUNCE_MS) continue;
        if (touchStableState[i] == touchRawState[i]) continue;

        touchStableState[i] = touchRawState[i];
        if (!touchStableState[i]) continue;  // Toggle only on touch press, not release

        const bool newState = !relayState[i];
        setRelay(i, newState);
        Serial.printf("[TOUCH]  Touch %d toggled Relay %d -> %s\n",
                      i + 1, i + 1, newState ? "ON" : "OFF");

        if (nodeState == STATE_PAIRED || nodeState == STATE_DISC_PEND || nodeState == STATE_GW_LOST) {
            sendActuatorState();
        }
    }
}

// *****************************************************************************
//  ESP-NOW callbacks
// *****************************************************************************
static void onDataRecv(const esp_now_recv_info_t* info,
                       const uint8_t* data, int len) {
    RxPacket pkt;
    memcpy(pkt.mac, info->src_addr, 6);
    int l = (len < 250) ? len : 250;
    memcpy(pkt.data, data, l);
    pkt.len = l;
    xQueueSend(rxQueue, &pkt, 0);
    // For Debugging: print received packet info to Serial Monitor
    Serial.printf("[RX]  Packet received from %02X:%02X:%02X:%02X:%02X:%02X  len=%d\n",
                  info->src_addr[0], info->src_addr[1], info->src_addr[2],
                  info->src_addr[3], info->src_addr[4], info->src_addr[5],
                  len);
}

static void onDataSent(const wifi_tx_info_t* /*txInfo*/, esp_now_send_status_t status) {
    if (!hasMaster) {
        // For Debugging
        Serial.println("[ESP-NOW] Send callback received but no master known. Ignoring.");
        return;
    }
    if (status == ESP_NOW_SEND_SUCCESS) {
        // For Debugging
        Serial.println("[ESP-NOW] Send successful.");
        txFailCount = 0;
    } 
    else {
        // For Debugging
        Serial.println("[ESP-NOW] Send failed.");
        if (txFailCount < 255) txFailCount = txFailCount + 1;
    }
}

// *****************************************************************************
//  NVS helpers
// *****************************************************************************
static bool loadPreferences() {
  prefs.begin("mesh", true); // read-only
  myNodeId  = prefs.getUChar("node_id",  0);
  myChannel = prefs.getUChar("channel",  0);
  size_t ml = prefs.getBytes("master_mac", masterMac, sizeof(masterMac));
  prefs.end();
  bool macValid = false;
  for (int i = 0; i < 6; i++) {
    if (masterMac[i] != 0) { 
      macValid = true;
    }
  }
  // For Debugging: print loaded preferences to Serial Monitor
  Serial.printf("[NVS] Loaded: id=%d  ch=%d\n", myNodeId, myChannel);
  return (myNodeId > 0 && myChannel > 0 && ml == 6 && macValid);
}

static void savePreferences() {
    prefs.begin("mesh", false); // read-write
    prefs.putUChar("node_id",  myNodeId);
    prefs.putUChar("channel",  myChannel);
    prefs.putBytes("master_mac", masterMac, 6);
    prefs.end();
    // For Debugging: print saved preferences to Serial Monitor
    Serial.printf("[NVS] Saved: id=%d  ch=%d\n", myNodeId, myChannel);
}

static void clearPreferences() {
    prefs.begin("mesh", false); // read-write
    prefs.clear();
    prefs.end();
    // For Debugging: print cleared preferences info to Serial Monitor
    Serial.println("[NVS] Cleared.");
}

// *****************************************************************************
//  Settings NVS load / save and definition helpers
// *****************************************************************************
static void loadSettings() {
    prefs.begin("nodeconf", true);

    sRelayPersist = prefs.getBool("relay_persist", false);
    sSettingLedEn = prefs.getBool("led_en", true);
  
    prefs.end();
    // For Debugging: print loaded settings to Serial Monitor
    Serial.printf("[CFG] Settings Loaded: relay_persist=%d led_en=%d\n", 
                  (int)sRelayPersist, (int)sSettingLedEn);
}

static void saveSettings() {
    prefs.begin("nodeconf", false);
    
    prefs.putBool("relay_persist", sRelayPersist);
    prefs.putBool("led_en", sSettingLedEn);

    prefs.end();
    // For Debugging: print saved settings to Serial Monitor
    Serial.printf("[CFG] Settings saved. Current values: relay_persist=%d led_en=%d\n", 
                  (int)sRelayPersist, (int)sSettingLedEn);
}

static uint8_t getSettingsDefs(SettingDef out[NODE_MAX_SETTINGS]) {
    uint8_t i = 0;

    // Relay State Persistence
    if (i >= NODE_MAX_SETTINGS) return i;
    out[i].id = SETTING_ID_RELAY_PERSIST;
    out[i].type = SETTING_BOOL;
    strncpy(out[i].label, "StatePersist", SETTING_LABEL_LEN - 1);
    out[i].label[SETTING_LABEL_LEN - 1] = '\0';
    out[i].current = sRelayPersist ? 1 : 0;
    out[i].i_min = 0;
    out[i].i_max = 0;
    out[i].i_step = 0;
    out[i].opt_count = 0;
    memset(out[i].opts, 0, sizeof(out[i].opts));
    i++;

    // Node Status LED
    if (i >= NODE_MAX_SETTINGS) return i;
    out[i].id = SETTING_ID_LED_EN;
    out[i].type = SETTING_BOOL;
    strncpy(out[i].label, "Status LED", SETTING_LABEL_LEN - 1);
    out[i].label[SETTING_LABEL_LEN - 1] = '\0';
    out[i].current = sSettingLedEn ? 1 : 0;
    out[i].i_min = 0; 
    out[i].i_max = 0; 
    out[i].i_step = 0;
    out[i].opt_count = 0;
    memset(out[i].opts, 0, sizeof(out[i].opts));
    i++;

    // For Debugging: print settings definitions to Serial Monitor
    Serial.printf("[CFG] Defined %d settings.\n", i);
    return i;
}

static uint8_t getActuatorDefs(ActuatorDef out[NODE_MAX_ACTUATORS])
{
    uint8_t i = 0;

    for (; i < RELAY_COUNT; i++) {
        out[i].id = i;
        strncpy(out[i].label, relayLabel[i], ACTUATOR_LABEL_LEN - 1);
        out[i].label[ACTUATOR_LABEL_LEN - 1] = '\0';
    }

    // For Debugging: print loaded actuator definitions to Serial Monitor
    Serial.printf("[ACTUATOR] Defined %d actuators.\n", i);
    return i;
}
// *****************************************************************************
//  ESP-NOW peer helpers
// *****************************************************************************
static void addMasterPeer() {
    if (esp_now_is_peer_exist(masterMac)) return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, masterMac, 6);
    p.channel = myChannel;
    p.encrypt = false;
    if (esp_now_add_peer(&p) == ESP_OK)
        Serial.println("[ESP-NOW] Master peer added.");
    else
        Serial.println("[ESP-NOW] Failed to add master peer!");
}

static void addBroadcastPeer(uint8_t channel) {
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (esp_now_is_peer_exist(bcast)) esp_now_del_peer(bcast);
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, bcast, 6);
    p.channel = channel;
    p.encrypt = false;
    esp_now_add_peer(&p);
    // For Debugging: print broadcast peer addition info to Serial Monitor
    Serial.printf("[ESP-NOW] Broadcast peer added on channel %d.\n", channel);
}

// *****************************************************************************
//  Message senders
// *****************************************************************************
static void sendBeacon() {
    MsgBeacon b;
    b.hdr.type      = MSG_BEACON;
    b.hdr.node_id   = 0;
    b.hdr.node_type = NODE_ACTUATOR;
    strncpy(b.name, NODE_NAME, 15);
    b.name[15]      = '\0';
    b.tx_channel    = pairingChannel;
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_send(bcast, (uint8_t*)&b, sizeof(b));
    Serial.printf("[PAIR]  Beacon -> ch%d\n", pairingChannel);
}

static void sendRegistration() {
    MsgRegister reg{};
    reg.hdr.type      = MSG_REGISTER;
    reg.hdr.node_id   = myNodeId;
    reg.hdr.node_type = NODE_ACTUATOR;
    strncpy(reg.name, NODE_NAME, 15);
    reg.name[15] = '\0';
    strncpy(reg.fw_version, FW_VERSION, 7);
    reg.fw_version[7] = '\0';
    strncpy(reg.hw_config_id, HW_CONFIG_ID, sizeof(reg.hw_config_id) - 1);
    reg.hw_config_id[sizeof(reg.hw_config_id) - 1] = '\0';
    reg.capabilities = NODE_CAP_ACTUATORS;
    esp_now_send(masterMac, (uint8_t*)&reg, sizeof(reg));
    // For Debugging: print sent registration info to Serial Monitor
    Serial.printf("[MSG]  Registration sent to master.\n Waiting for ACK...\n");
}

static void sendActuatorState()
{
    if (!hasMaster || myNodeId == 0) return;

    const uint8_t count = RELAY_COUNT;

    bool changed = false;

    for (uint8_t i = 0; i < count; i++)
    {
        if (relayState[i] != lastRelayState[i])
        {
            changed = true;
            break;
        }
    }

    if (!changed)
        return;

    MsgActuatorState msg{};

    msg.hdr.type = MSG_ACTUATOR_STATE;
    msg.hdr.node_id = myNodeId;
    msg.hdr.node_type = NODE_ACTUATOR;

    msg.count = count;

    for (uint8_t i = 0; i < count; i++)
    {
        msg.states[i].id = i;
        msg.states[i].state = relayState[i];
        lastRelayState[i] = relayState[i];
    }

    const size_t payloadLen =
        sizeof(MeshHeader) +
        sizeof(msg.count) +
        (count * sizeof(ActuatorState));

    esp_err_t r = esp_now_send(masterMac, (uint8_t*)&msg, payloadLen);
    // Flash LED on successful send only if LED is enabled in Node settings
    if (r == ESP_OK && sSettingLedEn) flashLed();

    // For Debugging: print sent actuator state info to Serial Monitor
    Serial.printf("[ACTUATOR]  Actuator state sent to master. %d relays, (%u B)\n", count, (unsigned)payloadLen);
    
    Serial.printf("  %s\n", r == ESP_OK ? "ok" : "error");
}

static void sendActuatorSchemaData()
{
    if(!hasMaster || myNodeId == 0) return;

    MsgActuatorSchema msg{};

    msg.hdr.type = MSG_ACTUATOR_SCHEMA;
    msg.hdr.node_id = myNodeId;
    msg.hdr.node_type = NODE_ACTUATOR;
    msg.count = getActuatorDefs(msg.actuators);

    const size_t payloadLen =
        sizeof(MeshHeader) +
        sizeof(msg.count) +
        msg.count * sizeof(ActuatorDef);

    esp_err_t r = esp_now_send(masterMac, (uint8_t*)&msg, payloadLen);

    // For Debugging: print sent actuator schema info to Serial Monitor
    Serial.printf("[ACTUATOR]  Sent actuator schema to master. %d actuator(s), (%u B) ->\n", msg.count, (unsigned)payloadLen);

    Serial.printf("  %s\n", r == ESP_OK ? "ok" : "error");
}

static void sendHeartbeat() {
    if (!hasMaster || myNodeId == 0) return;
    MsgHeartbeat hb;
    hb.hdr.type      = MSG_HEARTBEAT;
    hb.hdr.node_id   = myNodeId;
    hb.hdr.node_type = NODE_ACTUATOR;
    hb.uptime_sec    = millis() / 1000;
    
    esp_err_t r = esp_now_send(masterMac, (uint8_t*)&hb, sizeof(hb));
    // Flash LED on successful send only if LED is enabled in Node settings
    if (r == ESP_OK && sSettingLedEn) flashLed();
    
    // For Debugging: print sent heartbeat info to Serial Monitor
    Serial.printf("[ACTUATOR]  Heartbeat sent to master (uptime: %d sec) ->\n", hb.uptime_sec);
    Serial.printf("  %s\n", r == ESP_OK ? "ok" : "error");
}

static void sendSettingsData() {
    if (!hasMaster || myNodeId == 0) return;

    MsgSettingsData msg;
    msg.hdr.type      = MSG_SETTINGS_DATA;
    msg.hdr.node_id   = myNodeId;
    msg.hdr.node_type = NODE_ACTUATOR;
    msg.count         = getSettingsDefs(msg.settings);

    size_t payloadLen = sizeof(MeshHeader) + 1 + msg.count * sizeof(SettingDef);
    esp_err_t r = esp_now_send(masterMac, (uint8_t*)&msg, payloadLen);
    Serial.printf("[CFG]  Sent %d settings (%u B) ->\n", msg.count, (unsigned)payloadLen);
    Serial.printf("  %s\n", r == ESP_OK ? "ok" : "error");
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

static void sendOtaStatusNow(uint8_t phase, uint8_t progress, uint8_t errorCode, const char* message) {
    if (!hasMaster || myNodeId == 0 || pendingOta.sessionId == 0) return;

    MsgNodeOtaStatus status{};
    status.hdr.type = MSG_NODE_OTA_STATUS;
    status.hdr.node_id = myNodeId;
    status.hdr.node_type = NODE_ACTUATOR;
    status.session_id = pendingOta.sessionId;
    status.phase = phase;
    status.progress = progress;
    status.error_code = errorCode;
    if (message) strncpy(status.message, message, sizeof(status.message) - 1);
    esp_now_send(masterMac, reinterpret_cast<uint8_t*>(&status), sizeof(status));
}

static bool postOtaStatusToHelper(uint8_t phase, uint8_t progress, uint8_t errorCode, const char* message) {
    if (WiFi.status() != WL_CONNECTED || pendingOta.port == 0) return false;

    WiFiClient client;
    client.setTimeout(2000);
    HTTPClient http;
    String url = String("http://192.168.4.1:") + String(pendingOta.port) + "/status";
    if (!http.begin(client, url)) return false;
    http.setTimeout(2000);
    http.useHTTP10(true);

    JsonDocument doc;
    doc["session_id"] = pendingOta.sessionId;
    doc["node_id"] = myNodeId;
    doc["phase"] = phase;
    doc["progress"] = progress;
    doc["error_code"] = errorCode;
    doc["message"] = message ? message : "";

    String body;
    serializeJson(doc, body);
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST(body);
    Serial.printf("[OTA]  Helper status POST phase=%u result=%d\n", phase, code);
    http.end();
    return code == 200;
}

static void failNodeOta(const char* message, uint8_t progress = 100, uint8_t errorCode = 1) {
    Serial.printf("[OTA]  ERROR: %s\n", message ? message : "unknown");
    sendOtaStatusNow(NODE_OTA_ERROR, progress, errorCode, message ? message : "OTA failed");
    postOtaStatusToHelper(NODE_OTA_ERROR, progress, errorCode, message ? message : "OTA failed");
    delay(300);
    ESP.restart();
}

static void runPendingOta() {
    if (!pendingOta.pending || otaRunning) return;
    otaRunning = true;
    pendingOta.pending = false;

    sendOtaStatusNow(NODE_OTA_ACCEPTED, 5, 0, "Switching to OTA mode");
    delay(80);

    esp_now_deinit();
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.persistent(false);

    Serial.printf("[OTA]  Connecting to helper AP \"%s\"\n", pendingOta.ssid);
    WiFi.begin(pendingOta.ssid, pendingOta.password);

    const unsigned long connectStarted = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - connectStarted) < 20000) {
        delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
        failNodeOta("Could not connect to OTA helper AP.", 15, 2);
    }

    Serial.printf("[OTA]  Helper AP connected: local=%s gateway=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.RSSI());
    postOtaStatusToHelper(NODE_OTA_AP_CONNECTING, 20, 0, "Connected to OTA helper AP");
    delay(250);

    WiFiClient client;
    client.setTimeout(15000);
    HTTPClient http;
    const String url = String("http://192.168.4.1:") + String(pendingOta.port) + "/firmware.bin";
    if (!http.begin(client, url)) {
        failNodeOta("Could not open OTA HTTP session.", 25, 3);
    }

    http.setTimeout(15000);
    http.useHTTP10(true);
    Serial.printf("[OTA]  Downloading from %s\n", url.c_str());
    const int code = http.GET();
    Serial.printf("[OTA]  Firmware GET result: %d\n", code);
    if (code != HTTP_CODE_OK) {
        http.end();
        failNodeOta("Firmware download request failed.", 25, 4);
    }

    WiFiClient* stream = http.getStreamPtr();
    if (!Update.begin(pendingOta.imageSize)) {
        http.end();
        failNodeOta("Not enough OTA flash space on node.", 30, 5);
    }

    // Keep the helper HTTP connection dedicated to the firmware stream.

    uint8_t buffer[1024];
    size_t written = 0;
    uint32_t crc32 = 0xFFFFFFFFu;
    uint8_t lastProgress = 30;
    unsigned long lastDataAt = millis();

    while (written < pendingOta.imageSize) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (!http.connected()) {
                delay(20);
                avail = stream->available();
                if (avail == 0) break;
            }
            if ((millis() - lastDataAt) > 20000UL) {
                http.end();
                failNodeOta("Firmware stream timed out.", 80, 7);
            }
            delay(2);
            continue;
        }

        size_t toRead = pendingOta.imageSize - written;
        if (toRead > avail) toRead = avail;
        if (toRead > sizeof(buffer)) toRead = sizeof(buffer);
        const int got = stream->readBytes(reinterpret_cast<char*>(buffer), toRead);
        if (got <= 0) {
            if ((millis() - lastDataAt) > 20000UL) {
                http.end();
                failNodeOta("Firmware stream stalled.", 80, 7);
            }
            delay(2);
            continue;
        }

        lastDataAt = millis();
        if (Update.write(buffer, (size_t)got) != (size_t)got) {
            Serial.print("[OTA]  Update.write failed: ");
            Update.printError(Serial);
            http.end();
            failNodeOta("Writing firmware to flash failed.", 75, 6);
        }

        crc32 = crc32Update(crc32, buffer, (size_t)got);
        written += (size_t)got;

        const uint8_t progress = (uint8_t)(30 + ((written * 55) / pendingOta.imageSize));
        if (progress >= lastProgress + 5 || progress >= 85) {
            lastProgress = progress;
        }
    }
    Serial.printf("[OTA]  Download loop ended: wrote=%u/%u connected=%d available=%u\n",
                  (unsigned)written,
                  (unsigned)pendingOta.imageSize,
                  http.connected() ? 1 : 0,
                  (unsigned)stream->available());
    http.end();

    if (written != pendingOta.imageSize) {
        failNodeOta("Downloaded firmware size mismatch.", 85, 7);
    }
    if (crc32 != pendingOta.imageCrc32) {
        failNodeOta("Downloaded firmware checksum mismatch.", 88, 8);
    }

    Serial.println("[OTA]  Finalizing downloaded firmware...");
    if (!Update.end(true)) {
        Serial.print("[OTA]  Update.end failed: ");
        Update.printError(Serial);
        failNodeOta("Firmware finalization failed.", 95, 9);
    }
    if (!Update.isFinished()) {
        failNodeOta("Firmware finalization incomplete.", 95, 9);
    }

    Serial.println("[OTA]  Firmware finalized successfully. Rebooting...");
    WiFi.disconnect(true, true);
    delay(400);
    ESP.restart();
}

// *****************************************************************************
//  Disconnect helper
// *****************************************************************************
static void doDisconnect() {
    Serial.println("[PAIR]  Disconnecting...");
    if (hasMaster && esp_now_is_peer_exist(masterMac)) {
        MsgUnpairCmd msg;
        msg.hdr.type      = MSG_UNPAIR_CMD;
        msg.hdr.node_id   = myNodeId;
        msg.hdr.node_type = NODE_ACTUATOR;
        esp_now_send(masterMac, (uint8_t*)&msg, sizeof(msg));
        delay(80);
        esp_now_del_peer(masterMac);
    }
    clearPreferences();
    memset(masterMac, 0, 6);
    myNodeId    = 0;
    myChannel   = 0;
    hasMaster   = false;
    masterAcked = false;
    nodeState   = STATE_UNPAIRED;
    Serial.println("[PAIR]  Unpaired. Hold Pairing Button for 3-seconds to enter the pairing mode.");
}

// *****************************************************************************
//  Button handler
// *****************************************************************************
static void handleButton() {
    unsigned long now     = millis();
    bool          btnDown = (digitalRead(PAIR_BTN_PIN) == LOW);

    if (btnDown && !btnWasDown) {
        btnPressedAt = now;
        phase2Shown  = false;
    }

    if (btnDown) {
        unsigned long held = now - btnPressedAt;

        if (nodeState == STATE_UNPAIRED || nodeState == STATE_PAIRING) {
            if (nodeState == STATE_UNPAIRED && held >= 3000 && !phase2Shown) {
                phase2Shown    = true;
                nodeState      = STATE_PAIRING;
                pairingStarted = now;
                pairingChannel = 1;
                lastBeacon     = 0;
                addBroadcastPeer(pairingChannel);
                Serial.println("[PAIR]  Pairing mode - beaconing on ch 1-13...");
            }
        } else if (nodeState == STATE_PAIRED || nodeState == STATE_DISC_PEND || nodeState == STATE_GW_LOST) {
            if (held >= 3000 && !phase2Shown) {
                phase2Shown = true;
                nodeState   = STATE_DISC_PEND;
                Serial.println("[PAIR]  Hold Pairing Button for 2 more seconds to disconnect...");
            }
            if (held >= 5000) {
                doDisconnect();
            }
        }
    }

    if (!btnDown && btnWasDown) {
        unsigned long held = now - btnPressedAt;
        if (nodeState == STATE_DISC_PEND && held < 5000) {
            nodeState   = STATE_PAIRED;
            phase2Shown = false;
            Serial.println("[PAIR]  Disconnect cancelled.");
        }
    }

    btnWasDown = btnDown;
}

// *****************************************************************************
//  RX packet processor
// *****************************************************************************
static void processRxQueue() {
    RxPacket pkt;
    while (xQueueReceive(rxQueue, &pkt, 0) == pdTRUE) {
        if (pkt.len < (int)sizeof(MeshHeader)) continue;
        auto* hdr = (MeshHeader*)pkt.data;

        switch (hdr->type) {

            case MSG_PAIR_CMD: {
                if (pkt.len < (int)sizeof(MsgPairCmd)) break;
                if (nodeState != STATE_PAIRING) break;
                auto* cmd = (MsgPairCmd*)pkt.data;
                myChannel = cmd->channel;
                memcpy(masterMac, pkt.mac, 6);
                hasMaster = true;
                esp_wifi_set_channel(myChannel, WIFI_SECOND_CHAN_NONE);
                uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                if (esp_now_is_peer_exist(bcast)) esp_now_del_peer(bcast);
                addMasterPeer();
                myNodeId = 0;
                sendRegistration();
                Serial.printf("[PAIR]  PAIR_CMD received - ch=%d  master=%02X:%02X:%02X:%02X:%02X:%02X\n",
                              myChannel,
                              masterMac[0], masterMac[1], masterMac[2],
                              masterMac[3], masterMac[4], masterMac[5]);
                Serial.println("[PAIR]  Sent MSG_REGISTER, awaiting ACK...");
                break;
            }

            case MSG_REGISTER_ACK: {
                if (pkt.len < (int)sizeof(MsgRegisterAck)) break;
                auto* ack = (MsgRegisterAck*)pkt.data;
                masterAcked = true;
                hasMaster   = true;
                bool changed = (ack->assigned_id != myNodeId || ack->channel != myChannel);
                if (changed) {
                    myNodeId  = ack->assigned_id;
                    myChannel = ack->channel;
                    memcpy(masterMac, pkt.mac, 6);
                    esp_now_del_peer(masterMac);
                    esp_wifi_set_channel(myChannel, WIFI_SECOND_CHAN_NONE);
                    addMasterPeer();
                }
                savePreferences();
                txFailCount = 0;
                nodeState = STATE_PAIRED;
                for (uint8_t i = 0; i < RELAY_COUNT; i++) lastRelayState[i] = 255;
                sendActuatorState();
                Serial.printf("[PAIR]  Paired!  id=%d  ch=%d  master=%02X:%02X:%02X:%02X:%02X:%02X\n",
                              myNodeId, myChannel,
                              masterMac[0], masterMac[1], masterMac[2],
                              masterMac[3], masterMac[4], masterMac[5]);
                break;
            }

            case MSG_UNPAIR_CMD: {
                Serial.println("[PAIR]  UNPAIR_CMD received from gateway.");
                if (esp_now_is_peer_exist(masterMac)) esp_now_del_peer(masterMac);
                clearPreferences();
                memset(masterMac, 0, 6);
                myNodeId    = 0;
                myChannel   = 0;
                hasMaster   = false;
                masterAcked = false;
                nodeState   = STATE_UNPAIRED;
                break;
            }

            case MSG_REBOOT_CMD: {
                Serial.println("[CMD] Reboot command received from gateway.");
                delay(100);
                ESP.restart();
                break;
            }

            case MSG_SETTINGS_GET: {
                if (nodeState != STATE_PAIRED && nodeState != STATE_DISC_PEND) break;
                Serial.println("[CFG]  Settings GET received - sending schema.");
                sendSettingsData();
                break;
            }

            case MSG_ACTUATOR_SCHEMA_GET: {
                if (nodeState != STATE_PAIRED && nodeState != STATE_DISC_PEND) break;
                Serial.println("[ACTUATOR]  Actuator schema GET received - sending schema.");
                sendActuatorSchemaData();
                break;
            }

            case MSG_ACTUATOR_SET: {
                if(pkt.len < (int)sizeof(MsgActuatorSet)) break;
                auto* cmd = (MsgActuatorSet*)pkt.data;

                Serial.printf("[CMD] Actuator %d -> state %d\n",
                              cmd->actuator_id, cmd->state);

                setRelay(cmd->actuator_id, cmd->state);
                
                sendActuatorState();

                Serial.printf("[ACT] Relay %d -> state %d\n", cmd->actuator_id, cmd->state);

                break;
            }

            case MSG_SETTINGS_SET: {
                // Validate packet size before accessing payload
                if (pkt.len < (int)sizeof(MsgSettingsSet)) break;
                if (nodeState != STATE_PAIRED && nodeState != STATE_DISC_PEND) break;

                auto* ss = (MsgSettingsSet*)pkt.data;

                bool changed = false;

                switch (ss->id) {

                    case SETTING_ID_RELAY_PERSIST:
                        sRelayPersist = (ss->value != 0);
                        changed = true;
                        break;

                    case SETTING_ID_LED_EN:
                        sSettingLedEn = (ss->value != 0);
                        changed = true;
                        break;

                    default:
                        Serial.printf("[CFG] Unknown setting id %d - ignored\n", ss->id);
                        break;
                }

                if (changed) {
                    saveSettings();
                    if (ss->id == SETTING_ID_RELAY_PERSIST && sRelayPersist) {
                        for (uint8_t i = 0; i < RELAY_COUNT; i++) saveRelayState(i, relayState[i]);
                    }
                    updateLed();
                    Serial.printf("[CFG] Setting %d set to %d\n", ss->id, ss->value);
                    sendSettingsData();
                    sendActuatorSchemaData();   // keeps UI in sync
                }

                break;
            }

            case MSG_NODE_OTA_BEGIN: {
                if (pkt.len < (int)sizeof(MsgNodeOtaBegin)) break;
                if (nodeState != STATE_PAIRED && nodeState != STATE_DISC_PEND && nodeState != STATE_GW_LOST) break;

                auto* ota = (MsgNodeOtaBegin*)pkt.data;
                pendingOta = PendingNodeOta{};
                pendingOta.pending = true;
                pendingOta.sessionId = ota->session_id;
                pendingOta.imageSize = ota->image_size;
                pendingOta.imageCrc32 = ota->image_crc32;
                pendingOta.port = ota->port;
                strncpy(pendingOta.ssid, ota->ssid, sizeof(pendingOta.ssid) - 1);
                strncpy(pendingOta.password, ota->password, sizeof(pendingOta.password) - 1);
                strncpy(pendingOta.version, ota->version, sizeof(pendingOta.version) - 1);

                Serial.printf("[OTA]  Node OTA request received. version=%s size=%lu\n",
                              pendingOta.version, (unsigned long)pendingOta.imageSize);
                sendOtaStatusNow(NODE_OTA_ACCEPTED, 1, 0, "OTA request accepted");
                break;
            }

            default:
                break;
        }
    }
}

// *****************************************************************************
//  SETUP
// *****************************************************************************
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n[BOOT] %s starting...\n", NODE_NAME);
    touchFirmwareMarkers();

    led.begin();
    led.setBrightness(60);
    setLed(led.Color(255, 255, 255));
    delay(300);

    pinMode(PAIR_BTN_PIN, INPUT_PULLUP);
    
    if (relay_active_high) {
        // Initialize relay pins and set them to OFF (active HIGH)
        pinMode(RELAY1_PIN, OUTPUT); digitalWrite(RELAY1_PIN, LOW);
        pinMode(RELAY2_PIN, OUTPUT); digitalWrite(RELAY2_PIN, LOW);
        pinMode(RELAY3_PIN, OUTPUT); digitalWrite(RELAY3_PIN, LOW);
        pinMode(RELAY4_PIN, OUTPUT); digitalWrite(RELAY4_PIN, LOW);
    } 
    else {
        // Initialize relay pins and set them to OFF (active LOW)
        pinMode(RELAY1_PIN, OUTPUT); digitalWrite(RELAY1_PIN, HIGH);
        pinMode(RELAY2_PIN, OUTPUT); digitalWrite(RELAY2_PIN, HIGH);
        pinMode(RELAY3_PIN, OUTPUT); digitalWrite(RELAY3_PIN, HIGH);
        pinMode(RELAY4_PIN, OUTPUT); digitalWrite(RELAY4_PIN, HIGH);
    }

    // TTP224 outputs are digital push-pull lines. Treat HIGH as touched.
    pinMode(TOUCH1_PIN, INPUT);
    pinMode(TOUCH2_PIN, INPUT);
    pinMode(TOUCH3_PIN, INPUT);
    pinMode(TOUCH4_PIN, INPUT);
    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        const bool rawActive = (digitalRead(touchPins[i]) == HIGH);
        touchRawState[i] = rawActive;
        touchStableState[i] = rawActive;
        touchChangedAt[i] = millis();
    }

    // Default relay labels
    strcpy(relayLabel[0], "Relay 1");
    strcpy(relayLabel[1], "Relay 2");
    strcpy(relayLabel[2], "Relay 3");
    strcpy(relayLabel[3], "Relay 4");

    // Load settings, init ESP-NOW and attempt to pair with master if preferences are found in NVS
    loadSettings();

    // If relay state persistence is enabled, load the last known states and apply them to the relays
    if(sRelayPersist)
    {
        prefs.begin("relay", true);
        for(int i=0;i<RELAY_COUNT;i++)
        {
            char key[8];
            buildRelayStateKey(i, key, sizeof(key));
            relayState[i] = prefs.getBool(key, false);
        }
        prefs.end();

        for(int i=0;i<RELAY_COUNT;i++)
        {
            setRelay(i, relayState[i], false);
            // For Debugging: print loaded relay states to Serial Monitor
            Serial.printf("[NVS]  Loaded relay %d state: %s\n", i, relayState[i] ? "ON" : "OFF");
        }
        // For Debugging: print loaded relay states to Serial Monitor
        Serial.println("[NVS]  Loaded relay states from NVS Complete.");
    }

    rxQueue = xQueueCreate(10, sizeof(RxPacket));
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] Init failed - rebooting");
        delay(1000);
        ESP.restart();
    }
    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onDataSent);

    if (loadPreferences()) {
        Serial.printf("[NVS]  Loaded: id=%d  ch=%d\n", myNodeId, myChannel);
        esp_wifi_set_channel(myChannel, WIFI_SECOND_CHAN_NONE);
        hasMaster   = true;
        masterAcked = false;
        lastReReg   = millis();
        nodeState   = STATE_PAIRED;
        addMasterPeer();
        delay(200);
        sendRegistration();
        Serial.println("[NVS]  Re-registration sent, waiting for ACK...");
    } 
    else {
        Serial.println("[PAIR]  No pairing data - hold pairing button for 3-seconds to pair.");
        nodeState = STATE_UNPAIRED;
    }

    Serial.println("[BOOT] Setup complete.\n");
}
// *****************************************************************************
// LOOP
// *****************************************************************************
void loop() {
    unsigned long now = millis();

    runPendingOta();
    handleButton();
    handleTouchInputs();
    processRxQueue();

    if (nodeState == STATE_PAIRED && txFailCount >= GW_LOST_THRESHOLD) {
        nodeState   = STATE_GW_LOST;
        masterAcked = false;
        lastReReg   = now;
        txFailCount = 0;
        Serial.println("[MESH]  Gateway unreachable - pausing sends, re-registering...");
    }
    updateLed();

    if (nodeState == STATE_PAIRING) {
        if (now - pairingStarted >= PAIRING_TIMEOUT_MS) {
            nodeState = STATE_UNPAIRED;
            uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            if (esp_now_is_peer_exist(bcast)) esp_now_del_peer(bcast);
            Serial.println("[PAIR]  Timeout - exited pairing mode.");
        } 
        else if (now - lastBeacon >= BEACON_INTERVAL) {
            lastBeacon = now;
            esp_wifi_set_channel(pairingChannel, WIFI_SECOND_CHAN_NONE);
            addBroadcastPeer(pairingChannel);
            sendBeacon();
            pairingChannel = (pairingChannel % 13) + 1;
        }
    }

    if (nodeState == STATE_PAIRED || nodeState == STATE_DISC_PEND) {
        if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
            lastHeartbeat = now;
            sendHeartbeat();
        }
    }

    if ((nodeState == STATE_PAIRED || nodeState == STATE_GW_LOST) &&
        hasMaster && !masterAcked && (now - lastReReg >= 5000)) {
        lastReReg = now;
        Serial.println("[PAIR]  No ACK - re-sending registration...");
        sendRegistration();
    }

    delay(50);
}




