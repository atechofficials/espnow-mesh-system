#pragma once
/**
    * @file [mesh_protocol.h]
    * @brief Shared ESP-NOW message definitions for the ESP32 ESPNow Mesh System project
    * @version 3.3.1
    * @author Mrinal (@atechofficials)
    * @details Shared ESP-NOW message definitions
        * Copy this file into the include/ folder of every node project.
        *
        * All structs are packed (no compiler padding) so that the raw byte
        * layout is identical on every device, regardless of toolchain defaults.
        *
        * First-class Hybrid node support (v3.3.0) **********************************************
        * Nodes can now advertise capability flags during registration so the
        * gateway can request only the schemas/configuration they support.
        * Dedicated RFID configuration / scan-event messages were added for the
        * first Hybrid actuator + RFID node family.
        *
        * Node Type changed from RELAY to ACTUATOR (v3.1.0) **********************************************
        * Node Type RELAY is now ACTUATOR to better reflect the role of nodes that
        * control relays, lights, or other outputs. This is a breaking change for
        * node firmware (must update to v3.1.0), but it better supports future
        * non-relay actuators.
        *
        * Scalable sensor readings (v3.0.0) **********************************************
        * Sensor readings are now fully schema-driven, mirroring the settings system.
        * Nodes self-describe their sensors once via MSG_SENSOR_SCHEMA (on request or
        * proactively after a setting that changes a unit is applied). Live readings
        * then flow as a generic id->value array in MSG_SENSOR_DATA.
        *
        * The gateway never hardcodes knowledge of specific sensors. Adding a new
        * sensor to a node requires ONLY node firmware changes - the gateway and web
        * interface pick it up automatically.
        *
        * Breaking change from (v2.0.0) **********************************************
        * MsgSensorData is NOT backward-compatible with v2.0.0 (named temperature /
        * pressure fields replaced by a generic readings array). All node firmware
        * must be updated to v3.0.0 before connecting to a v3.0.0 gateway.
 */

#include <stdint.h>

#define SW_VERSION "3.3.1"

// Message Types *****************************************************************************
typedef enum : uint8_t {
    MSG_REGISTER            = 0x01,  // Node    -> Master    : request registration / re-register after reboot
    MSG_REGISTER_ACK        = 0x02,  // Master  -> Node      : assign ID + confirm channel
    MSG_SENSOR_DATA         = 0x03,  // Node    -> Master    : generic sensor readings array
    MSG_HEARTBEAT           = 0x04,  // Node    -> Master    : keepalive
    MSG_BEACON              = 0x05,  // Node    -> broadcast : announce availability (pairing mode)
    MSG_PAIR_CMD            = 0x06,  // Master  -> Node      : "connect to me on this channel"
    MSG_UNPAIR_CMD          = 0x07,  // Master <-> Node      : bidirectional disconnect + clear NVS
    MSG_REBOOT_CMD          = 0x08,  // Master  -> Node      : reboot the node remotely
    MSG_SETTINGS_GET        = 0x09,  // Master  -> Node      : request settings schema + current values
    MSG_SETTINGS_DATA       = 0x0A,  // Node    -> Master    : settings schema + current values
    MSG_SETTINGS_SET        = 0x0B,  // Master  -> Node      : update a single setting value
    MSG_SENSOR_SCHEMA_GET   = 0x0C,  // Master  -> Node      : request sensor schema
    MSG_SENSOR_SCHEMA       = 0x0D,  // Node    -> Master    : sensor schema (label / unit / precision per sensor)
    MSG_ACTUATOR_SCHEMA_GET = 0x0E,  // Master  -> Node      : request actuator schema
    MSG_ACTUATOR_SCHEMA     = 0x0F,  // Node    -> Master    : actuator schema
    MSG_ACTUATOR_STATE      = 0x10,  // Node    -> Master    : actuator states
    MSG_ACTUATOR_SET        = 0x11,  // Master  -> Node      : set actuator state
    MSG_NODE_OTA_BEGIN      = 0x12,  // Master  -> Node      : enter node OTA mode and fetch firmware over Wi-Fi
    MSG_NODE_OTA_STATUS     = 0x13,  // Node    -> Master    : OTA state / progress for one node
    MSG_RFID_CONFIG_GET     = 0x14,  // Master  -> Node      : request RFID card configuration table
    MSG_RFID_CONFIG_DATA    = 0x15,  // Node    -> Master    : RFID card configuration table
    MSG_RFID_CONFIG_SET     = 0x16,  // Master  -> Node      : create/update/clear one RFID card slot
    MSG_RFID_SCAN_EVENT     = 0x17,  // Node    -> Master    : report that an RFID card was scanned
} MeshMsgType;

// Node Types *****************************************************************************
typedef enum : uint8_t {
    NODE_SENSOR   = 0x01,
    NODE_ACTUATOR = 0x02,
    NODE_HYBRID   = 0x03,
} NodeType;

// Node Capability Flags *****************************************************************************
#define NODE_CAP_SENSOR_DATA  0x00000001UL
#define NODE_CAP_ACTUATORS    0x00000002UL
#define NODE_CAP_RFID         0x00000004UL

// Setting Types *****************************************************************************
typedef enum : uint8_t {
    SETTING_BOOL   = 0x00,  // toggle - value is 0 or 1
    SETTING_ENUM   = 0x01,  // picker - value is option index 0..opt_count-1
    SETTING_INT    = 0x02,  // number - value is i_min..i_max in i_step increments
    SETTING_STRING = 0x03,  // string - UTF-8 value transmitted in MsgSettingsSet.data
} SettingType;

// Settings Constants *****************************************************************************
// NODE_MAX_SETTINGS caps one ESP-NOW packet:
//   sizeof(MeshHeader)+1 + NODE_MAX_SETTINGS*sizeof(SettingDef)
//   = 3 + 1 + 4*55 = 224 bytes (limit 250 bytes for ESP-NOW)
#define NODE_MAX_SETTINGS     4
#define SETTING_LABEL_LEN    12
#define SETTING_OPT_MAXCOUNT  4
#define SETTING_OPT_LEN       8

// Sensor Constants *****************************************************************************
// NODE_MAX_SENSORS caps one ESP-NOW packet for schema and data:
//   MsgSensorSchema : sizeof(MeshHeader)+1 + NODE_MAX_SENSORS*sizeof(SensorDef)
//                   = 3 + 1 + 8*20 = 164 bytes
//   MsgSensorData   : sizeof(MeshHeader)+5 + NODE_MAX_SENSORS*sizeof(SensorReading)
//                   = 3 + 5 + 8*5  = 48 bytes
#define NODE_MAX_SENSORS     8
#define SENSOR_LABEL_LEN    12
#define SENSOR_UNIT_LEN      6

// Actuator Constants *****************************************************************************
// NODE_MAX_ACTUATORS caps one ESP-NOW packet for schema and data:
//   MsgActuatorSchema : sizeof(MeshHeader)+1 + NODE_MAX_ACTUATORS*sizeof(ActuatorDef)
//                     = 3 + 1 + 8*13 = 108 bytes
//   MsgActuatorState  : sizeof(MeshHeader)+1 + NODE_MAX_ACTUATORS*sizeof(ActuatorState)
//                     = 3 + 1 + 8*2  = 20 bytes
#define NODE_MAX_ACTUATORS  8
#define ACTUATOR_LABEL_LEN 12

// RFID Constants *****************************************************************************
#define RFID_MAX_SLOTS   8
#define RFID_UID_MAX_LEN 10

// Hardware Configuration Constants *****************************************************************************
#define HW_CONFIG_ID_LEN 12
#define MESH_NODE_NAME_LEN 25   // 24 visible chars + NUL

// Node OTA Constants *****************************************************************************
#define NODE_OTA_SSID_LEN     16
#define NODE_OTA_PASS_LEN     16
#define NODE_OTA_VERSION_LEN  16
#define NODE_OTA_MESSAGE_LEN  24

typedef enum : uint8_t {
    NODE_OTA_IDLE          = 0x00,
    NODE_OTA_ACCEPTED      = 0x01,
    NODE_OTA_AP_CONNECTING = 0x02,
    NODE_OTA_DOWNLOADING   = 0x03,
    NODE_OTA_FLASHING      = 0x04,
    NODE_OTA_SUCCESS       = 0x05,
    NODE_OTA_ERROR         = 0x06,
} NodeOtaPhase;

// Packed Structs *****************************************************************************
#pragma pack(push, 1)

typedef struct {
    MeshMsgType type;
    uint8_t     node_id;    // 0 = unassigned (during pairing/registration)
    NodeType    node_type;
} MeshHeader;

typedef struct {
    MeshHeader hdr;
    char       name[MESH_NODE_NAME_LEN];
    char       fw_version[8];
    char       hw_config_id[HW_CONFIG_ID_LEN];
    uint32_t   capabilities;
} MsgRegister;

#define MSG_REGISTER_MIN_LEN   (sizeof(MeshHeader) + MESH_NODE_NAME_LEN + 8)
#define MSG_REGISTER_HWCFG_LEN (MSG_REGISTER_MIN_LEN + HW_CONFIG_ID_LEN)
#define MSG_REGISTER_CAPS_LEN  (MSG_REGISTER_HWCFG_LEN + sizeof(uint32_t))

typedef struct {
    MeshHeader hdr;
    uint8_t    assigned_id;
    uint8_t    channel;
} MsgRegisterAck;

typedef struct {
    uint8_t id;
    float   value;
} SensorReading;

typedef struct {
    MeshHeader    hdr;
    uint32_t      uptime_sec;
    uint8_t       count;
    SensorReading readings[NODE_MAX_SENSORS];
} MsgSensorData;

typedef struct {
    uint8_t id;
    uint8_t state;
} ActuatorState;

typedef struct {
    MeshHeader    hdr;
    uint8_t       count;
    ActuatorState states[NODE_MAX_ACTUATORS];
} MsgActuatorState;

typedef struct {
    MeshHeader hdr;
    uint8_t    actuator_id;
    uint8_t    state;
} MsgActuatorSet;

typedef struct {
    MeshHeader hdr;
    uint32_t   uptime_sec;
} MsgHeartbeat;

typedef struct {
    MeshHeader hdr;
    char       name[MESH_NODE_NAME_LEN];
    uint8_t    tx_channel;
} MsgBeacon;

typedef struct {
    MeshHeader hdr;
    uint8_t    channel;
} MsgPairCmd;

typedef struct {
    MeshHeader hdr;
} MsgUnpairCmd;

typedef struct {
    MeshHeader hdr;
} MsgRebootCmd;

typedef struct {
    uint8_t     id;
    SettingType type;
    char        label[SETTING_LABEL_LEN];
    int16_t     current;
    int16_t     i_min;
    int16_t     i_max;
    int16_t     i_step;
    uint8_t     opt_count;
    char        opts[SETTING_OPT_MAXCOUNT][SETTING_OPT_LEN];
} SettingDef;

typedef struct {
    MeshHeader hdr;
} MsgSettingsGet;

typedef struct {
    MeshHeader hdr;
    uint8_t    count;
    SettingDef settings[NODE_MAX_SETTINGS];
} MsgSettingsData;

typedef struct {
    MeshHeader hdr;
    uint8_t    id;
    int16_t    value;
    char       data[16];
} MsgSettingsSet;

typedef struct {
    uint8_t id;
    uint8_t precision;
    char    label[SENSOR_LABEL_LEN];
    char    unit[SENSOR_UNIT_LEN];
} SensorDef;

typedef struct {
    uint8_t id;
    char    label[ACTUATOR_LABEL_LEN];
} ActuatorDef;

typedef struct {
    uint8_t  slot;
    uint8_t  enabled;
    uint8_t  uid_len;
    uint8_t  reserved;
    uint16_t relay_mask;
    uint8_t  uid[RFID_UID_MAX_LEN];
} RfidSlotDef;

typedef struct {
    MeshHeader hdr;
} MsgSensorSchemaGet;

typedef struct {
    MeshHeader hdr;
    uint8_t    count;
    SensorDef  sensors[NODE_MAX_SENSORS];
} MsgSensorSchema;

typedef struct {
    MeshHeader hdr;
} MsgActuatorSchemaGet;

typedef struct {
    MeshHeader hdr;
    uint8_t    count;
    ActuatorDef actuators[NODE_MAX_ACTUATORS];
} MsgActuatorSchema;

typedef struct {
    MeshHeader hdr;
} MsgRfidConfigGet;

typedef struct {
    MeshHeader hdr;
    uint8_t    count;
    RfidSlotDef slots[RFID_MAX_SLOTS];
} MsgRfidConfigData;

typedef struct {
    MeshHeader hdr;
    RfidSlotDef slot;
} MsgRfidConfigSet;

typedef struct {
    MeshHeader hdr;
    uint8_t    uid_len;
    uint8_t    matched_slot;
    uint16_t   applied_relay_mask;
    uint8_t    uid[RFID_UID_MAX_LEN];
} MsgRfidScanEvent;

typedef struct {
    MeshHeader hdr;
    uint32_t   session_id;
    uint32_t   image_size;
    uint32_t   image_crc32;
    uint16_t   port;
    char       ssid[NODE_OTA_SSID_LEN];
    char       password[NODE_OTA_PASS_LEN];
    char       version[NODE_OTA_VERSION_LEN];
} MsgNodeOtaBegin;

typedef struct {
    MeshHeader hdr;
    uint32_t   session_id;
    uint8_t    phase;
    uint8_t    progress;
    uint8_t    error_code;
    char       message[NODE_OTA_MESSAGE_LEN];
} MsgNodeOtaStatus;

#pragma pack(pop)

// Shared Constants *****************************************************************************
#define MESH_MAX_NODES        20
#define MESH_MAX_ACTUATORS    NODE_MAX_ACTUATORS
#define NODE_TIMEOUT_MS       35000UL
#define HEARTBEAT_INTERVAL    30000UL
#define SENSOR_INTERVAL       10000UL
#define BEACON_INTERVAL       2000UL
#define PAIRING_TIMEOUT_MS    60000UL
#define DISCOVERED_TIMEOUT_MS 60000UL
#define PAIR_CMD_RETRY_MS     2000UL
#define PAIR_CMD_TIMEOUT_MS   30000UL
