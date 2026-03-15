#pragma once
/**
    * @file [mesh_protocol.h]
    * @brief Shared ESP-NOW message definitions for the ESP32 ESPNow Mesh System project
    * @version 3.1.0
    * @author Mrinal (@atechofficials)
    * @details Shared ESP-NOW message definitions
        * Copy this file into the include/ folder of every node project.
        *
        * All structs are packed (no compiler padding) so that the raw byte
        * layout is identical on every device, regardless of toolchain defaults.
        *
        * Node Type changed from RELAY to ACTUATOR (v3.1.0) **********************************************
        * Node Type RELAY is now ACTUATOR to better reflect the role of nodes that 
        * control relays, lights, or other outputs. This is a breaking change for 
        * node firmware (must update to v3.1.0), but it better supports future 
        * non-relay actuators.
        * Added a new NODE_HYBRID type for future use (e.g. combined sensor+actuator nodes),
        * but it's not currently used.
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

#define SW_VERSION "3.1.0"

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
    MSG_SETTINGS_SET        = 0x0B,  // Master   -> Node     : update a single setting value
    MSG_SENSOR_SCHEMA_GET   = 0x0C,  // Master   -> Node     : request sensor schema
    MSG_SENSOR_SCHEMA       = 0x0D,  // Node     -> Master   : sensor schema (label / unit / precision per sensor)
    MSG_ACTUATOR_SCHEMA_GET = 0x0E,  // Master   -> Node     : request actuator schema (e.g. relay count)
    MSG_ACTUATOR_SCHEMA     = 0x0F,  // Node     -> Master   : actuator schema (e.g. relay count)
    MSG_ACTUATOR_STATE      = 0x10,  // Node     -> Master   : actuator states (e.g. relay states)
    MSG_ACTUATOR_SET        = 0x11,  // Master   -> Node     : set actuator state (e.g. toggle relay)
} MeshMsgType;

// Node Types *****************************************************************************
typedef enum : uint8_t {
    NODE_SENSOR = 0x01,
    NODE_ACTUATOR = 0x02,
    NODE_HYBRID = 0x03,  // reserved for future use (e.g. combined sensor+actuator nodes)
} NodeType;

// Setting Types *****************************************************************************
typedef enum : uint8_t {
    SETTING_BOOL = 0x00,  // toggle - value is 0 or 1
    SETTING_ENUM = 0x01,  // picker - value is option index 0..opt_count-1
    SETTING_INT  = 0x02,  // number - value is i_min..i_max in i_step increments
    SETTING_STRING = 0x03, // string - UTF-8 value transmitted in MsgSettingsSet.data
} SettingType;

// Settings Constants *****************************************************************************
// NODE_MAX_SETTINGS caps one ESP-NOW packet:
//   sizeof(MeshHeader)+1 + NODE_MAX_SETTINGS×sizeof(SettingDef)
//   = 3 + 1 + 4×55 = 224 bytes  (limit 250 bytes for ESP-NOW)
#define NODE_MAX_SETTINGS    4   // max settings transmitted per packet
#define SETTING_LABEL_LEN   12   // chars for human-readable label (incl. NUL)
#define SETTING_OPT_MAXCOUNT 4   // max enum options per setting
#define SETTING_OPT_LEN      8   // chars per enum option string (incl. NUL)

// Sensor Constants *****************************************************************************
// NODE_MAX_SENSORS caps one ESP-NOW packet for schema and data:
//   MsgSensorSchema : sizeof(MeshHeader)+1 + NODE_MAX_SENSORS×sizeof(SensorDef)
//                   = 3 + 1 + 8×20 = 164 bytes  (limit 250 bytes for ESP-NOW)
//   MsgSensorData   : sizeof(MeshHeader)+5 + NODE_MAX_SENSORS×sizeof(SensorReading)
//                   = 3 + 5 + 8×5  =  48 bytes  (limit 250 bytes for ESP-NOW)
#define NODE_MAX_SENSORS    8    // max sensor readings per node
#define SENSOR_LABEL_LEN   12   // chars for human-readable sensor label (incl. NUL)
#define SENSOR_UNIT_LEN     6   // chars for unit string e.g. "°C", "%RH" (incl. NUL)

// Actuator Constants *****************************************************************************
// NODE_MAX_ACTUATORS caps one ESP-NOW packet for schema and data:
//   MsgActuatorSchema : sizeof(MeshHeader)+1 + NODE_MAX_ACTUATORS×sizeof(ActuatorDef)
//                     = 3 + 1 + 4×13 = 56 bytes  (limit 250 bytes for ESP-NOW)
//   MsgActuatorState  : sizeof(MeshHeader)+1 + NODE_MAX_ACTUATORS×sizeof(ActuatorState)
//                     = 3 + 1 + 4×2  = 12 bytes  (limit 250 bytes for ESP-NOW)
#define NODE_MAX_ACTUATORS 4    // max actuators (e.g. relays) per node
#define ACTUATOR_LABEL_LEN 12   // chars for human-readable actuator label (incl. NUL)

// Packed Structs *****************************************************************************
#pragma pack(push, 1)

// Common 3-byte header - every message starts with this.
typedef struct {
    MeshMsgType type;
    uint8_t     node_id;    // 0 = unassigned (during pairing/registration)
    NodeType    node_type;
} MeshHeader;

// MSG_REGISTER (Node -> Master) *****************************************************************************
typedef struct {
    MeshHeader hdr;
    char       name[16];
    char       fw_version[8];   // e.g. "1.2.0\0" - populated from FW_VERSION define
} MsgRegister;

// MSG_REGISTER_ACK (Master -> Node) *****************************************************************************
typedef struct {
    MeshHeader hdr;
    uint8_t    assigned_id;
    uint8_t    channel;
} MsgRegisterAck;

// MSG_SENSOR_DATA (Node -> Master) *****************************************************************************
// Generic readings packet - carries only the sensors the node has measured this
// cycle. All values are float; the matching SensorDef (sent once via
// MSG_SENSOR_SCHEMA) supplies the label, unit, and display precision.
//
// Packed size: sizeof(MeshHeader)=3 + uint32_t=4 + uint8_t=1
//              + count×sizeof(SensorReading)=count×5
// Maximum (8 sensors): 48 bytes - well within the 250-byte ESP-NOW limit.
typedef struct {
    uint8_t id;     // matches SensorDef.id sent in MSG_SENSOR_SCHEMA
    float   value;  // the measured value in the unit declared by SensorDef.unit
} SensorReading;    // 5 bytes packed

typedef struct {
    MeshHeader    hdr;
    uint32_t      uptime_sec;
    uint8_t       count;                        // number of valid entries (1..NODE_MAX_SENSORS)
    SensorReading readings[NODE_MAX_SENSORS];   // only first `count` entries are valid
} MsgSensorData;

// MSG_ACTUATOR_STATE (Node -> Master) *****************************************************************************
typedef struct {
    uint8_t id;
    uint8_t state;
} ActuatorState;   // 2 bytes packed

typedef struct {
    MeshHeader hdr;
    uint8_t    count;                          // number of valid entries (1..NODE_MAX_ACTUATORS)
    ActuatorState states[NODE_MAX_ACTUATORS];  // only first `count` entries are valid
} MsgActuatorState;

// MSG_ACTUATOR_SET (Gateway -> Node) *****************************************************************************
// Command to set actuator state (relay on/off)

typedef struct {
    MeshHeader hdr;
    uint8_t actuator_id;   // relay index (0..3)
    uint8_t state;         // 0 = OFF, 1 = ON
} MsgActuatorSet;

// MSG_HEARTBEAT (Node -> Master) *****************************************************************************
typedef struct {
    MeshHeader hdr;
    uint32_t   uptime_sec;
} MsgHeartbeat;

// MSG_BEACON (Node -> broadcast, pairing mode only) *****************************************************************************
// tx_channel: the WiFi channel this beacon was sent on.
// Gateway uses this to add the node as a peer on the correct channel
// so MSG_PAIR_CMD retries can reach it.
typedef struct {
    MeshHeader hdr;        // node_id=0, node_type=actual
    char       name[16];
    uint8_t    tx_channel;
} MsgBeacon;

// MSG_PAIR_CMD (Master -> Node) *****************************************************************************
// Sent when user clicks "Connect" on the gateway dashboard.
// Node responds by switching channel and sending MSG_REGISTER.
typedef struct {
    MeshHeader hdr;
    uint8_t    channel;
} MsgPairCmd;

// MSG_UNPAIR_CMD (Master <-> Node, bidirectional) *****************************************************************************
// Master -> Node : user clicked "Disconnect" in dashboard.
// Node   -> Master: user held pairing button for 5-seconds.
typedef struct {
    MeshHeader hdr;
} MsgUnpairCmd;

// MSG_REBOOT_CMD (Master -> Node) *****************************************************************************
// Sent when user clicks "Reboot" in the dashboard for a specific node.
// Node calls ESP.restart() on receipt; it will re-register automatically via NVS.
typedef struct {
    MeshHeader hdr;
} MsgRebootCmd;

// SettingDef (embedded in MSG_SETTINGS_DATA) *****************************************************************************
// Fixed-size descriptor for a single configurable setting.
// Packed: 1+1+12+2+2+2+2+1+(4×8) = 55 bytes per entry.
//
//  BOOL : current = 0|1 (i_min/max/step and opts[] are unused)
//  ENUM : current = selected option index 0..opt_count-1 (i_min/max/step unused)
//  INT  : current is the live value; i_min/i_max/i_step define the allowed range
typedef struct {
    uint8_t     id;                                          // unique per-node ID (0–7)
    SettingType type;                                        // BOOL / ENUM / INT
    char        label[SETTING_LABEL_LEN];                    // display name
    int16_t     current;                                     // current value
    int16_t     i_min;                                       // INT only: minimum
    int16_t     i_max;                                       // INT only: maximum
    int16_t     i_step;                                      // INT only: step size
    uint8_t     opt_count;                                   // ENUM only: # options
    char        opts[SETTING_OPT_MAXCOUNT][SETTING_OPT_LEN]; // ENUM only: strings
} SettingDef;   // 55 bytes packed

// MSG_SETTINGS_GET (Master -> Node) *****************************************************************************
// Gateway sends this after registration to request the node's settings schema.
// Node must respond with MSG_SETTINGS_DATA.
typedef struct {
    MeshHeader hdr;
} MsgSettingsGet;

// MSG_SETTINGS_DATA (Node -> Master) *****************************************************************************
// Node sends its full settings schema + current values.
// Only `count` entries in settings[] are valid; send exactly:
//   sizeof(MeshHeader) + 1 + count * sizeof(SettingDef)  bytes.
// Maximum packet: 3 + 1 + 4×55 = 224 bytes  (within 250-byte ESP-NOW limit).
typedef struct {
    MeshHeader hdr;
    uint8_t    count;                        // number of valid entries (0..NODE_MAX_SETTINGS)
    SettingDef settings[NODE_MAX_SETTINGS];  // only first `count` entries matter
} MsgSettingsData;

// MSG_SETTINGS_SET (Master -> Node) *****************************************************************************
// Gateway sends this when the user changes a setting in the dashboard.
// Node must apply, optionally persist, then echo back MSG_SETTINGS_DATA.
// If the changed setting affects a sensor unit (e.g. °C <-> °F), the node
// SHOULD also proactively send an updated MSG_SENSOR_SCHEMA so the gateway
// and web interface immediately reflect the new unit without waiting for the
// next MSG_SENSOR_SCHEMA_GET request.
typedef struct {
    MeshHeader hdr;
    uint8_t    id;     // SettingDef.id to update
    int16_t    value;  // new value (bool:0/1, enum:index, int:value)
    char data[16];   // used only for SETTING_STRING type - UTF-8 bytes of the new string value (max 15 chars + NUL)
} MsgSettingsSet;

// SensorDef (embedded in MSG_SENSOR_SCHEMA) *****************************************************************************
// Fixed-size descriptor for one sensor reading channel.
// Packed: 1+1+12+6 = 20 bytes per entry.
//
// The node assigns a stable id (0-based) to each sensor.  The gateway stores
// the schema and uses it to label and format every MSG_SENSOR_DATA reading.
// All values are transmitted as float; precision controls display rounding.
typedef struct {
    uint8_t id;                         // unique per-node sensor index (0..NODE_MAX_SENSORS-1)
    uint8_t precision;                  // decimal places for display (0–4)
    char    label[SENSOR_LABEL_LEN];    // human-readable name, e.g. "Temperature"
    char    unit[SENSOR_UNIT_LEN];      // unit string,  e.g. "°C", "%RH", "hPa", "%"
} SensorDef;    // 20 bytes packed

// ActuatorDef (embedded in MSG_ACTUATOR_SCHEMA) *****************************************************************************
typedef struct {
    uint8_t id;                         // unique per-node actuator index (0..NODE_MAX_ACTUATORS-1)
    char label[ACTUATOR_LABEL_LEN];  // human-readable name, e.g. "Relay 1"
} ActuatorDef;  // 13 bytes packed


// MSG_SENSOR_SCHEMA_GET (Master -> Node) *****************************************************************************
// Gateway sends this after registration to request the node's sensor schema.
// Node must respond with MSG_SENSOR_SCHEMA.
typedef struct {
    MeshHeader hdr;
} MsgSensorSchemaGet;

// MSG_SENSOR_SCHEMA (Node -> Master) *****************************************************************************
// Node sends its full sensor schema.  Only `count` entries in sensors[] are
// valid; the node should send exactly:
//   sizeof(MeshHeader) + 1 + count * sizeof(SensorDef)  bytes.
// Maximum packet: 3 + 1 + 8×20 = 164 bytes  (within 250-byte ESP-NOW limit).
//
// Send this:
//   In response to MSG_SENSOR_SCHEMA_GET.
//   Proactively after applying any setting that changes a sensor unit
//     (e.g. Temp Unit °C <-> °F), so the gateway reflects the new unit
//     without the user needing to reload the dashboard.
typedef struct {
    MeshHeader hdr;
    uint8_t    count;                       // number of valid entries (0..NODE_MAX_SENSORS)
    SensorDef  sensors[NODE_MAX_SENSORS];   // only first `count` entries matter
} MsgSensorSchema;

// MSG_ACTUATOR_SCHEMA_GET (Master -> Node) *****************************************************************************
typedef struct {
    MeshHeader hdr;
} MsgActuatorSchemaGet;

// MSG_ACTUATOR_SCHEMA  (Node -> Master) *****************************************************************************
typedef struct {
    MeshHeader hdr;
    uint8_t    count;                            // number of valid entries (0..NODE_MAX_ACTUATORS)
    ActuatorDef actuators[NODE_MAX_ACTUATORS];   // only first `count` entries matter
} MsgActuatorSchema;

#pragma pack(pop)

// Shared Constants *****************************************************************************
#define MESH_MAX_NODES          20
#define MESH_MAX_ACTUATORS      4
#define NODE_TIMEOUT_MS         35000UL     // if no message received within this time, node is considered offline (must be longer than HEARTBEAT_INTERVAL)
#define HEARTBEAT_INTERVAL      30000UL
#define SENSOR_INTERVAL         10000UL
#define BEACON_INTERVAL         2000UL      // beacon broadcast period (pairing mode)
#define PAIRING_TIMEOUT_MS      60000UL     // pairing mode auto-cancel
#define DISCOVERED_TIMEOUT_MS   60000UL     // remove from discovered list (must survive full 13-ch beacon cycle ~26 s)
#define PAIR_CMD_RETRY_MS       2000UL      // gateway PAIR_CMD retry interval
#define PAIR_CMD_TIMEOUT_MS     30000UL     // gateway gives up pairing attempt