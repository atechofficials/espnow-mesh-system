#pragma once

/**
    * @file [coproc_ota_protocol.h]
    * @brief Shared definitions for the OTA protocol between main MCU and the coprocessor.
    * @version 1.0.0
    * @author Mrinal (@atechofficials)
 */

#include <stdint.h>

#define SW_VERSION "1.0.0"

#define COPROC_FRAME_MAGIC 0xA55A
#define COPROC_STATUS_MESSAGE_LEN 40
#define COPROC_FW_VERSION_LEN 16
#define COPROC_SSID_LEN 16
#define COPROC_PASS_LEN 16
#define COPROC_UPLOAD_CHUNK_DATA_LEN 512

typedef enum : uint8_t {
    COPROC_FRAME_HELLO        = 0x01,
    COPROC_FRAME_ACK          = 0x02,
    COPROC_FRAME_UPLOAD_BEGIN = 0x10,
    COPROC_FRAME_UPLOAD_CHUNK = 0x11,
    COPROC_FRAME_UPLOAD_END   = 0x12,
    COPROC_FRAME_ABORT        = 0x13,
    COPROC_FRAME_STATUS       = 0x20,
} CoprocFrameType;

typedef enum : uint8_t {
    COPROC_HELPER_IDLE           = 0x00,
    COPROC_HELPER_RECEIVING      = 0x01,
    COPROC_HELPER_AP_READY       = 0x02,
    COPROC_HELPER_NODE_ACTIVE    = 0x03,
    COPROC_HELPER_DONE           = 0x04,
    COPROC_HELPER_ERROR          = 0x05,
} CoprocHelperPhase;

#pragma pack(push, 1)

typedef struct {
    uint16_t magic;
    uint8_t  type;
    uint16_t length;
} CoprocFrameHeader;

typedef struct {
    uint8_t  original_type;
    uint8_t  ok;
    uint32_t value;
    char     message[COPROC_STATUS_MESSAGE_LEN];
} CoprocAckPayload;

typedef struct {
    uint32_t session_id;
    uint32_t image_size;
    uint32_t image_crc32;
    uint8_t  node_type;
    uint8_t  ap_channel;
    uint16_t port;
    char     version[COPROC_FW_VERSION_LEN];
    char     ssid[COPROC_SSID_LEN];
    char     password[COPROC_PASS_LEN];
} CoprocUploadBeginPayload;

typedef struct {
    uint32_t offset;
    uint8_t  data[COPROC_UPLOAD_CHUNK_DATA_LEN];
} CoprocUploadChunkPayload;

typedef struct {
    uint32_t session_id;
    uint8_t  node_id;
    uint8_t  phase;
    uint8_t  progress;
    uint8_t  error_code;
    char     message[COPROC_STATUS_MESSAGE_LEN];
} CoprocStatusPayload;

#pragma pack(pop)



