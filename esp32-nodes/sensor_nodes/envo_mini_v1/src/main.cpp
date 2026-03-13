/**
 * ╔══════════════════════════════════════════════════════════════════════════╗
 * ║         ESP32 MESH — Sensor Node (BMP280 + DHT22 + TEMT6000)            ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  Boot sequence:                                                          ║
 * ║  1. Load saved master MAC + channel from NVS (Preferences).             ║
 * ║     If found  → set channel, add peer, re-register to confirm.          ║
 * ║     If NOT found → scan channels 1-13 broadcasting MSG_BEACON           ║
 * ║                    until master replies with MSG_PAIR_CMD.              ║
 * ║  2. Every SENSOR_INTERVAL ms  → read sensors, send MSG_SENSOR_DATA.    ║
 * ║  3. Every HEARTBEAT_INTERVAL ms → send MSG_HEARTBEAT.                   ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  BMP280 wiring (I2C):                                                   ║
 * ║     SDA → GPIO21   SCL → GPIO22   VCC → 3.3 V   GND → GND             ║
 * ║     SDO → GND  → I2C addr 0x76  |  SDO → VCC  → I2C addr 0x77        ║
 * ║  DHT22 wiring:                                                          ║
 * ║     DATA → GPIO16   VCC → 3.3 V   GND → GND                           ║
 * ║     (10 kΩ pull-up between DATA and VCC recommended)                   ║
 * ║  TEMT6000 wiring:                                                       ║
 * ║     OUT  → GPIO36   VCC → 3.3 V   GND → GND                           ║
 * ║     (GPIO36 is input-only ADC1_CH0 — do not drive it high)             ║
 * ╚══════════════════════════════════════════════════════════════════════════╝
 *
 * Sensor readings are fully schema-driven (v3.0 protocol).  The gateway and
 * web interface discover what sensors this node has via MSG_SENSOR_SCHEMA —
 * no gateway firmware changes are needed when sensors are added or removed.
 *
 * Change NODE_NAME below to give each sensor node a unique label.
 */
#define FW_VERSION "2.0.0"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>
#include <Adafruit_NeoPixel.h>
#include "mesh_protocol.h"

// ─── User config ──────────────────────────────────────────────────────────────
#define NODE_NAME       "BMP280-Node-1"   // ← change per node (max 15 chars)
#define BMP_I2C_SDA     21
#define BMP_I2C_SCL     22
#define BMP_ADDR_PRIM   0x76
#define BMP_ADDR_SEC    0x77
#define DHT_PIN         16    // DHT22 data pin
#define DHT_TYPE        DHT22
#define TEMT6000_PIN    36    // TEMT6000 analog output (ADC1_CH0 — input only)
#define PAIR_BTN_PIN    27    // Pairing button GPIO — active-LOW, uses internal pull-up
#define LED_PIN         5     // WS2812B data pin
#define LED_COUNT       1

// ─── Node state machine ───────────────────────────────────────────────────────
enum NodeState { STATE_UNPAIRED, STATE_PAIRING, STATE_PAIRED, STATE_DISC_PEND, STATE_GW_LOST };
static NodeState nodeState = STATE_UNPAIRED;

// ─── Globals ──────────────────────────────────────────────────────────────────
static uint8_t  masterMac[6] = {0};
static uint8_t  myNodeId     = 0;
static uint8_t  myChannel    = 0;
static bool     hasMaster    = false;
static bool     masterAcked  = false;
static unsigned long lastReReg = 0;

static Adafruit_BMP280   bmp;
static DHT               dht(DHT_PIN, DHT_TYPE);
static Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
static Preferences       prefs;

// ── Sensor presence — set during setup(), read-only thereafter ───────────────
static bool bmpOk  = false;
static bool dhtOk  = false;
static bool temtOk = false;

// ─── LED state ────────────────────────────────────────────────────────────────
static uint32_t     ledCurrent    = 0xDEADBEEF;
static unsigned long ledFlashUntil = 0;

// ─── Button state ─────────────────────────────────────────────────────────────
static bool          btnWasDown      = false;
static unsigned long btnPressedAt    = 0;
static bool          phase2Shown     = false;

// ─── Pairing state ────────────────────────────────────────────────────────────
static unsigned long pairingStarted  = 0;
static uint8_t       pairingChannel  = 1;
static unsigned long lastBeacon      = 0;

// ─── Timing ───────────────────────────────────────────────────────────────────
static unsigned long lastSensor      = 0;
static unsigned long lastHeartbeat   = 0;

// ─── Node Settings ────────────────────────────────────────────────────────────
#define SETTING_ID_TEMP_UNIT   0
#define SETTING_ID_SEND_INTVL  1
#define SETTING_ID_LED_EN      2
#define SETTING_ID_TEMT_SENS   3

static uint8_t  sSettingTempUnit  = 0;
static uint16_t sSettingSendIntvl = SENSOR_INTERVAL / 1000;
static bool     sSettingLedEn     = true;
static uint8_t  sSettingTemtSens  = 4;

// ── Gateway-loss detection ─────────────────────────────────────────────────
#define GW_LOST_THRESHOLD   3
static volatile uint8_t txFailCount = 0;

// ── RX queue ──────────────────────────────────────────────────────────────────
struct RxPacket { uint8_t mac[6]; uint8_t data[250]; int len; };
static QueueHandle_t rxQueue;

// ─────────────────────────────────────────────────────────────────────────────
//  LED helpers
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
//  ESP-NOW callbacks
// ─────────────────────────────────────────────────────────────────────────────
static void onDataRecv(const esp_now_recv_info_t* info,
                       const uint8_t* data, int len) {
    RxPacket pkt;
    memcpy(pkt.mac, info->src_addr, 6);
    int l = (len < 250) ? len : 250;
    memcpy(pkt.data, data, l);
    pkt.len = l;
    xQueueSend(rxQueue, &pkt, 0);
}

static void onDataSent(const wifi_tx_info_t* /*txInfo*/, esp_now_send_status_t status) {
    if (!hasMaster) return;
    if (status == ESP_NOW_SEND_SUCCESS) {
        txFailCount = 0;
    } else {
        if (txFailCount < 255) txFailCount = txFailCount + 1;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  NVS helpers
// ─────────────────────────────────────────────────────────────────────────────
static bool loadPreferences() {
    prefs.begin("mesh", true);
    myNodeId  = prefs.getUChar("node_id",  0);
    myChannel = prefs.getUChar("channel",  0);
    size_t ml = prefs.getBytes("master_mac", masterMac, 6);
    prefs.end();
    return (myNodeId > 0 && myChannel > 0 && ml == 6);
}

static void savePreferences() {
    prefs.begin("mesh", false);
    prefs.putUChar("node_id",  myNodeId);
    prefs.putUChar("channel",  myChannel);
    prefs.putBytes("master_mac", masterMac, 6);
    prefs.end();
    Serial.printf("[NVS]  Saved: id=%d  ch=%d\n", myNodeId, myChannel);
}

static void clearPreferences() {
    prefs.begin("mesh", false);
    prefs.clear();
    prefs.end();
    Serial.println("[NVS]  Cleared.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Settings — NVS load / save
// ─────────────────────────────────────────────────────────────────────────────
static void loadSettings() {
    prefs.begin("nodeconf", true);
    sSettingTempUnit  = prefs.getUChar("temp_unit",   0);
    sSettingSendIntvl = prefs.getUShort("send_intvl", SENSOR_INTERVAL / 1000);
    sSettingLedEn     = prefs.getBool("led_en",       true);
    sSettingTemtSens  = prefs.getUChar("temt_sens",   5);
    prefs.end();
    if (sSettingSendIntvl < 5)  sSettingSendIntvl = 5;
    if (sSettingSendIntvl > 60) sSettingSendIntvl = 60;
    if (sSettingTemtSens < 1)   sSettingTemtSens = 1;
    if (sSettingTemtSens > 10)  sSettingTemtSens = 10;
    Serial.printf("[CFG]  Settings: temp_unit=%d  send_intvl=%ds  led_en=%d  temt_sens=%d\n",
                  sSettingTempUnit, sSettingSendIntvl, (int)sSettingLedEn, sSettingTemtSens);
}

static void saveSettings() {
    prefs.begin("nodeconf", false);
    prefs.putUChar("temp_unit",  sSettingTempUnit);
    prefs.putUShort("send_intvl", sSettingSendIntvl);
    prefs.putBool("led_en",      sSettingLedEn);
    prefs.putUChar("temt_sens",  sSettingTemtSens);
    prefs.end();
    Serial.println("[CFG]  Settings saved.");
}

static uint8_t getSettingsDefs(SettingDef out[NODE_MAX_SETTINGS]) {
    uint8_t i = 0;

    out[i].id        = SETTING_ID_TEMP_UNIT;
    out[i].type      = SETTING_ENUM;
    strncpy(out[i].label, "Temp Unit", SETTING_LABEL_LEN - 1);
    out[i].label[SETTING_LABEL_LEN - 1] = '\0';
    out[i].current   = (int16_t)sSettingTempUnit;
    out[i].i_min     = 0; out[i].i_max = 0; out[i].i_step = 0;
    out[i].opt_count = 2;
    strncpy(out[i].opts[0], "\xC2\xB0""C", SETTING_OPT_LEN - 1); out[i].opts[0][SETTING_OPT_LEN-1] = '\0';
    strncpy(out[i].opts[1], "\xC2\xB0""F", SETTING_OPT_LEN - 1); out[i].opts[1][SETTING_OPT_LEN-1] = '\0';
    i++;

    out[i].id        = SETTING_ID_SEND_INTVL;
    out[i].type      = SETTING_INT;
    strncpy(out[i].label, "Send Intvl", SETTING_LABEL_LEN - 1);
    out[i].label[SETTING_LABEL_LEN - 1] = '\0';
    out[i].current   = (int16_t)sSettingSendIntvl;
    out[i].i_min     = 5; out[i].i_max = 60; out[i].i_step = 5;
    out[i].opt_count = 0;
    memset(out[i].opts, 0, sizeof(out[i].opts));
    i++;

    out[i].id        = SETTING_ID_LED_EN;
    out[i].type      = SETTING_BOOL;
    strncpy(out[i].label, "Status LED", SETTING_LABEL_LEN - 1);
    out[i].label[SETTING_LABEL_LEN - 1] = '\0';
    out[i].current   = sSettingLedEn ? 1 : 0;
    out[i].i_min     = 0; out[i].i_max = 0; out[i].i_step = 0;
    out[i].opt_count = 0;
    memset(out[i].opts, 0, sizeof(out[i].opts));
    i++;

    if (temtOk) {
        out[i].id        = SETTING_ID_TEMT_SENS;
        out[i].type      = SETTING_INT;
        strncpy(out[i].label, "Light Sens", SETTING_LABEL_LEN - 1);
        out[i].label[SETTING_LABEL_LEN - 1] = '\0';
        out[i].current   = (int16_t)sSettingTemtSens;
        out[i].i_min     = 1; out[i].i_max = 10; out[i].i_step = 1;
        out[i].opt_count = 0;
        memset(out[i].opts, 0, sizeof(out[i].opts));
        i++;
    }

    return i;
}

static uint8_t getSensorDefs(SensorDef out[NODE_MAX_SENSORS]) {
    uint8_t i = 0;
    const char* tempUnit = (sSettingTempUnit == 1) ? "\xC2\xB0""F" : "\xC2\xB0""C";

    if (bmpOk) {
        out[i].id        = 0;
        out[i].precision = 1;
        strncpy(out[i].label, "Temperature", SENSOR_LABEL_LEN - 1);
        out[i].label[SENSOR_LABEL_LEN - 1] = '\0';
        strncpy(out[i].unit, tempUnit, SENSOR_UNIT_LEN - 1);
        out[i].unit[SENSOR_UNIT_LEN - 1] = '\0';
        i++;

        out[i].id        = 1;
        out[i].precision = 1;
        strncpy(out[i].label, "Atm.Pressure", SENSOR_LABEL_LEN - 1);
        out[i].label[SENSOR_LABEL_LEN - 1] = '\0';
        strncpy(out[i].unit, "hPa", SENSOR_UNIT_LEN - 1);
        out[i].unit[SENSOR_UNIT_LEN - 1] = '\0';
        i++;
    }

    if (dhtOk) {
        out[i].id        = 2;
        out[i].precision = 1;
        strncpy(out[i].label, "Humidity", SENSOR_LABEL_LEN - 1);
        out[i].label[SENSOR_LABEL_LEN - 1] = '\0';
        strncpy(out[i].unit, "%", SENSOR_UNIT_LEN - 1);
        out[i].unit[SENSOR_UNIT_LEN - 1] = '\0';
        i++;
    }

    if (temtOk) {
        out[i].id        = 3;
        out[i].precision = 0;
        strncpy(out[i].label, "Ambi. Light", SENSOR_LABEL_LEN - 1);
        out[i].label[SENSOR_LABEL_LEN - 1] = '\0';
        strncpy(out[i].unit, "%", SENSOR_UNIT_LEN - 1);
        out[i].unit[SENSOR_UNIT_LEN - 1] = '\0';
        i++;
    }

    return i;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ESP-NOW peer helpers
// ─────────────────────────────────────────────────────────────────────────────
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
}

// ─────────────────────────────────────────────────────────────────────────────
//  Message senders
// ─────────────────────────────────────────────────────────────────────────────
static void sendBeacon() {
    MsgBeacon b;
    b.hdr.type      = MSG_BEACON;
    b.hdr.node_id   = 0;
    b.hdr.node_type = NODE_SENSOR;
    strncpy(b.name, NODE_NAME, 15);
    b.name[15]      = '\0';
    b.tx_channel    = pairingChannel;
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_send(bcast, (uint8_t*)&b, sizeof(b));
    Serial.printf("[PAIR]  Beacon → ch%d\n", pairingChannel);
}

static void sendRegistration() {
    MsgRegister reg;
    reg.hdr.type      = MSG_REGISTER;
    reg.hdr.node_id   = myNodeId;
    reg.hdr.node_type = NODE_SENSOR;
    strncpy(reg.name, NODE_NAME, 15);
    reg.name[15] = '\0';
    strncpy(reg.fw_version, FW_VERSION, 7);
    reg.fw_version[7] = '\0';
    esp_now_send(masterMac, (uint8_t*)&reg, sizeof(reg));
}

static void sendSensorData() {
    if (!hasMaster || myNodeId == 0) return;

    MsgSensorData sd;
    sd.hdr.type      = MSG_SENSOR_DATA;
    sd.hdr.node_id   = myNodeId;
    sd.hdr.node_type = NODE_SENSOR;
    sd.uptime_sec    = millis() / 1000;
    sd.count         = 0;

    if (bmpOk) {
        float tempC = bmp.readTemperature();
        float tempOut = (sSettingTempUnit == 1) ? (tempC * 9.0f / 5.0f + 32.0f) : tempC;
        sd.readings[sd.count++] = { .id = 0, .value = tempOut };
        sd.readings[sd.count++] = { .id = 1, .value = bmp.readPressure() / 100.0f };
    }

    if (dhtOk) {
        float h = dht.readHumidity();
        if (!isnan(h)) {
            sd.readings[sd.count++] = { .id = 2, .value = h };
        }
    }

    if (temtOk) {
        // ESP32 ADC is corrupted by WiFi/ESP-NOW TX bursts, producing spurious
        // zeros. Simple averaging still returns 0 when most samples land during
        // a TX window. Fix: collect 64 samples, discard zeros, average the rest.
        // This isolates only valid ADC conversions regardless of TX timing.
        const int SAMPLES = 64;
        long sum = 0;
        int  valid = 0;
        for (int s = 0; s < SAMPLES; s++) {
            int v = analogRead(TEMT6000_PIN);
            if (v > 0) { sum += v; valid++; }
        }
        int raw = (valid > 0) ? (int)(sum / valid) : 0;
        // Logarithmic scaling: lux is perceptually logarithmic, so a linear
        // formula maps indoor light to near-zero even though it looks bright.
        // log10(1+raw)/log10(4096) gives ~40% for typical room light (raw≈30)
        // and 100% for direct bright light (raw≈4095) at default sensitivity=5.
        // Sensitivity shifts the entire curve: sens=1 → dim, sens=10 → very sensitive.
        float normalized = (raw > 0) ? (log10f(1.0f + raw) / log10f(4096.0f)) : 0.0f;
        float pct = normalized * (sSettingTemtSens / 5.0f) * 100.0f;
        if (pct > 100.0f) pct = 100.0f;
        sd.readings[sd.count++] = { .id = 3, .value = pct };
    }

    size_t payloadLen = sizeof(MeshHeader) + sizeof(uint32_t) + 1
                        + sd.count * sizeof(SensorReading);
    esp_err_t r = esp_now_send(masterMac, (uint8_t*)&sd, payloadLen);

    Serial.printf("[SENS] Sent %u reading(s) (%u B) →", sd.count, (unsigned)payloadLen);
    for (uint8_t i = 0; i < sd.count; i++)
        Serial.printf("  [%d]=%.2f", sd.readings[i].id, sd.readings[i].value);
    Serial.printf("  %s\n", r == ESP_OK ? "ok" : "error");

    if (r == ESP_OK && sSettingLedEn) flashLed();
}

static void sendSensorSchemaData() {
    if (!hasMaster || myNodeId == 0) return;

    MsgSensorSchema msg;
    msg.hdr.type      = MSG_SENSOR_SCHEMA;
    msg.hdr.node_id   = myNodeId;
    msg.hdr.node_type = NODE_SENSOR;
    msg.count         = getSensorDefs(msg.sensors);

    size_t payloadLen = sizeof(MeshHeader) + 1 + msg.count * sizeof(SensorDef);
    esp_err_t r = esp_now_send(masterMac, (uint8_t*)&msg, payloadLen);
    Serial.printf("[SENS] Sent schema: %d sensor(s) (%u B) → %s\n",
                  msg.count, (unsigned)payloadLen,
                  r == ESP_OK ? "ok" : "error");
}

static void sendHeartbeat() {
    if (!hasMaster || myNodeId == 0) return;
    MsgHeartbeat hb;
    hb.hdr.type      = MSG_HEARTBEAT;
    hb.hdr.node_id   = myNodeId;
    hb.hdr.node_type = NODE_SENSOR;
    hb.uptime_sec    = millis() / 1000;
    if (esp_now_send(masterMac, (uint8_t*)&hb, sizeof(hb)) == ESP_OK) flashLed();
}

static void sendSettingsData() {
    if (!hasMaster || myNodeId == 0) return;

    MsgSettingsData msg;
    msg.hdr.type      = MSG_SETTINGS_DATA;
    msg.hdr.node_id   = myNodeId;
    msg.hdr.node_type = NODE_SENSOR;
    msg.count         = getSettingsDefs(msg.settings);

    size_t payloadLen = sizeof(MeshHeader) + 1 + msg.count * sizeof(SettingDef);
    esp_err_t r = esp_now_send(masterMac, (uint8_t*)&msg, payloadLen);
    Serial.printf("[CFG]  Sent %d settings (%u B) → %s\n",
                  msg.count, (unsigned)payloadLen,
                  r == ESP_OK ? "ok" : "error");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Disconnect
// ─────────────────────────────────────────────────────────────────────────────
static void doDisconnect() {
    Serial.println("[PAIR]  Disconnecting…");
    if (hasMaster && esp_now_is_peer_exist(masterMac)) {
        MsgUnpairCmd msg;
        msg.hdr.type      = MSG_UNPAIR_CMD;
        msg.hdr.node_id   = myNodeId;
        msg.hdr.node_type = NODE_SENSOR;
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
    Serial.println("[PAIR]  Unpaired. Hold button 3 s to enter pairing mode.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Button handler
// ─────────────────────────────────────────────────────────────────────────────
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
                Serial.println("[PAIR]  Pairing mode — beaconing on ch 1-13…");
            }
        } else if (nodeState == STATE_PAIRED || nodeState == STATE_DISC_PEND || nodeState == STATE_GW_LOST) {
            if (held >= 3000 && !phase2Shown) {
                phase2Shown = true;
                nodeState   = STATE_DISC_PEND;
                Serial.println("[PAIR]  Hold 2 more seconds to disconnect…");
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

// ─────────────────────────────────────────────────────────────────────────────
//  RX packet processor
// ─────────────────────────────────────────────────────────────────────────────
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
                Serial.printf("[PAIR]  PAIR_CMD received — ch=%d  master=%02X:%02X:%02X:%02X:%02X:%02X\n",
                              myChannel,
                              masterMac[0], masterMac[1], masterMac[2],
                              masterMac[3], masterMac[4], masterMac[5]);
                Serial.println("[PAIR]  Sent MSG_REGISTER, awaiting ACK…");
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
                Serial.printf("[PAIR]  ✓ Paired!  id=%d  ch=%d  master=%02X:%02X:%02X:%02X:%02X:%02X\n",
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
                Serial.println("[CFG]  Settings GET received — sending schema.");
                sendSettingsData();
                break;
            }

            case MSG_SENSOR_SCHEMA_GET: {
                if (nodeState != STATE_PAIRED && nodeState != STATE_DISC_PEND) break;
                Serial.println("[SENS]  Sensor schema GET received — sending schema.");
                sendSensorSchemaData();
                break;
            }

            case MSG_SETTINGS_SET: {
                if (pkt.len < (int)sizeof(MsgSettingsSet)) break;
                if (nodeState != STATE_PAIRED && nodeState != STATE_DISC_PEND) break;
                auto* ss = (MsgSettingsSet*)pkt.data;

                bool changed     = false;
                bool unitChanged = false;
                switch (ss->id) {
                    case SETTING_ID_TEMP_UNIT:
                        if (ss->value == 0 || ss->value == 1) {
                            sSettingTempUnit = (uint8_t)ss->value;
                            changed     = true;
                            unitChanged = true;
                        }
                        break;
                    case SETTING_ID_SEND_INTVL:
                        if (ss->value >= 5 && ss->value <= 60) {
                            sSettingSendIntvl = (uint16_t)ss->value;
                            changed = true;
                        }
                        break;
                    case SETTING_ID_LED_EN:
                        sSettingLedEn = (ss->value != 0);
                        changed = true;
                        break;
                    case SETTING_ID_TEMT_SENS:
                        if (ss->value >= 1 && ss->value <= 10) {
                            sSettingTemtSens = (uint8_t)ss->value;
                            changed = true;
                        }
                        break;
                    default:
                        Serial.printf("[CFG]  Unknown setting id %d — ignored\n", ss->id);
                        break;
                }

                if (changed) {
                    saveSettings();
                    Serial.printf("[CFG]  Setting %d set to %d\n", ss->id, ss->value);
                    sendSettingsData();
                    if (unitChanged) sendSensorSchemaData();
                }
                break;
            }

            default:
                break;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n[BOOT] %s starting…\n", NODE_NAME);

    led.begin();
    led.setBrightness(60);
    setLed(led.Color(255, 255, 255));
    delay(300);

    pinMode(PAIR_BTN_PIN, INPUT_PULLUP);

    // ── BMP280 ────────────────────────────────────────────────────────────────
    Wire.begin(BMP_I2C_SDA, BMP_I2C_SCL);
    bmpOk = bmp.begin(BMP_ADDR_PRIM);
    if (!bmpOk) bmpOk = bmp.begin(BMP_ADDR_SEC);
    if (!bmpOk) {
        Serial.println("[BMP]  WARNING: sensor not found — check wiring!");
    } else {
        bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                        Adafruit_BMP280::SAMPLING_X2,
                        Adafruit_BMP280::SAMPLING_X16,
                        Adafruit_BMP280::FILTER_X4,
                        Adafruit_BMP280::STANDBY_MS_250);
        Serial.println("[BMP]  Sensor ready.");
    }

    // ── DHT22 ─────────────────────────────────────────────────────────────────
    dht.begin();
    delay(2000);
    float testH = dht.readHumidity();
    dhtOk = !isnan(testH);
    Serial.printf("[DHT]  %s\n", dhtOk ? "Sensor ready." : "WARNING: no valid reading — check wiring!");

    // ── TEMT6000 ──────────────────────────────────────────────────────────────
    analogSetAttenuation(ADC_11db);
    analogRead(TEMT6000_PIN);
    temtOk = true;
    Serial.println("[TEMT] TEMT6000 ADC ready.");

    // ── Load settings, init ESP-NOW ───────────────────────────────────────────
    loadSettings();
    rxQueue = xQueueCreate(10, sizeof(RxPacket));
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] Init failed — rebooting");
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
        Serial.println("[NVS]  Re-registration sent, waiting for ACK…");
    } else {
        Serial.println("[PAIR]  No pairing data — hold button 3 s to pair.");
        nodeState = STATE_UNPAIRED;
    }

    Serial.println("[BOOT] Setup complete.\n");
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    handleButton();
    processRxQueue();

    if (nodeState == STATE_PAIRED && txFailCount >= GW_LOST_THRESHOLD) {
        nodeState   = STATE_GW_LOST;
        masterAcked = false;
        lastReReg   = now;
        txFailCount = 0;
        Serial.println("[MESH]  Gateway unreachable — pausing sends, re-registering...");
    }
    updateLed();

    if (nodeState == STATE_PAIRING) {
        if (now - pairingStarted >= PAIRING_TIMEOUT_MS) {
            nodeState = STATE_UNPAIRED;
            uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            if (esp_now_is_peer_exist(bcast)) esp_now_del_peer(bcast);
            Serial.println("[PAIR]  Timeout — exited pairing mode.");
        } else if (now - lastBeacon >= BEACON_INTERVAL) {
            lastBeacon = now;
            esp_wifi_set_channel(pairingChannel, WIFI_SECOND_CHAN_NONE);
            addBroadcastPeer(pairingChannel);
            sendBeacon();
            pairingChannel = (pairingChannel % 13) + 1;
        }
    }

    if (nodeState == STATE_PAIRED || nodeState == STATE_DISC_PEND) {
        if (now - lastSensor >= (unsigned long)sSettingSendIntvl * 1000UL) {
            lastSensor = now;
            sendSensorData();
        }
        if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
            lastHeartbeat = now;
            sendHeartbeat();
        }
    }

    if ((nodeState == STATE_PAIRED || nodeState == STATE_GW_LOST) &&
        hasMaster && !masterAcked && (now - lastReReg >= 5000)) {
        lastReReg = now;
        Serial.println("[PAIR]  No ACK — re-sending registration...");
        sendRegistration();
    }

    delay(50);
}