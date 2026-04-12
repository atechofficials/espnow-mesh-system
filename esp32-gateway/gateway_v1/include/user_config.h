#pragma once

/**
    * @file [user_config.h]
    * @brief Shared definitions for the ESP32 Mesh Gateway Firmware
    * @version 1.1.3
    * @author Mrinal (@atechofficials)
 */

// Gateway User Configurations
#define AP_SSID_DEFAULT  "ESP32-Mesh-Gateway"
#define AP_PASS_DEFAULT  "meshsetup"
#define OFFLINE_AP_SSID_DEFAULT "ESP32-Mesh-Offline"
#define OFFLINE_AP_PASS_DEFAULT "meshoffline"
#define OFFLINE_AP_DEFAULT_CHANNEL 6
#define NODE_OTA_HOST "192.168.4.1"

// Optional gateway built-in room environment sensor feature.
// 0 = disabled, 1 = enabled. When enabled, firmware auto-detects BMP280 vs BME280 at runtime.
#ifndef GATEWAY_BUILTIN_SENSOR_ENABLED
    #define GATEWAY_BUILTIN_SENSOR_ENABLED 0 // Default to disabled if not defined from platformio.ini build_flags.
#endif

// Select Main Dev Board
// These can be provided from platformio.ini build_flags per environment.
#if !defined(ESP32_S3_DEVKITC1) && !defined(XIAO_ESP32_S3)
    #define ESP32_S3_DEVKITC1
#endif

#if defined(ESP32_S3_DEVKITC1) && defined(XIAO_ESP32_S3)
    #error "Select only one gateway main dev board."
#endif

// Select Coprocessor Dev Board
// These can be provided from platformio.ini build_flags per environment.
#if !defined(COPROC_ESP32C3_BEETLE) && !defined(COPROC_ESP32C3_XIAO) && !defined(COPROC_ESP32C3_SUPER_MINI)
    #define COPROC_ESP32C3_BEETLE
#endif

#if (defined(COPROC_ESP32C3_BEETLE) && defined(COPROC_ESP32C3_XIAO)) || \
    (defined(COPROC_ESP32C3_BEETLE) && defined(COPROC_ESP32C3_SUPER_MINI)) || \
    (defined(COPROC_ESP32C3_XIAO) && defined(COPROC_ESP32C3_SUPER_MINI))
    #error "Select only one gateway coprocessor dev board."
#endif

#ifdef ESP32_S3_DEVKITC1
    #ifdef COPROC_ESP32C3_BEETLE
        #define HW_CONFIG_ID "0x0A"
    #elif defined(COPROC_ESP32C3_XIAO)
        #define HW_CONFIG_ID "0x1A"
    #elif defined(COPROC_ESP32C3_SUPER_MINI)
        #define HW_CONFIG_ID "0x2A"
    #endif
    #define RESET_BTN_PIN    7
    #define GW_LED_PIN       38
    #define GW_LED_COL_ORDER NEO_RGB
    #define COPROC_UART_TX_PIN 4
    #define COPROC_UART_RX_PIN 5
    #define COPROC_RESET_PIN 6
    #define BME_SCK  12
    #define BME_MISO 14
    #define BME_MOSI 11
    #define BME_CS   10
#elif defined (XIAO_ESP32_S3)
    #ifdef COPROC_ESP32C3_BEETLE
        #define HW_CONFIG_ID "0x3A"
    #elif defined(COPROC_ESP32C3_XIAO)
        #define HW_CONFIG_ID "0x4A"
    #elif defined(COPROC_ESP32C3_SUPER_MINI)
        #define HW_CONFIG_ID "0x5A"
    #endif
    #define RESET_BTN_PIN    6
    #define GW_LED_PIN       3
    #define GW_LED_COL_ORDER NEO_GRB
    #define COPROC_UART_TX_PIN 4
    #define COPROC_UART_RX_PIN 5
    #define COPROC_RESET_PIN 1
    #define BME_SCK  7
    #define BME_MISO 8
    #define BME_MOSI 9
    #define BME_CS   2
#endif

#if (GATEWAY_BUILTIN_SENSOR_ENABLED != 0) && (GATEWAY_BUILTIN_SENSOR_ENABLED != 1)
    #error "GATEWAY_BUILTIN_SENSOR_ENABLED must be 0 or 1."
#endif

// Coprocessor board-specific hardware config ID used by coprocessor OTA firmware validation.
// This intentionally identifies the ESP32-C3 helper board itself, not the full gateway combo.
#ifdef COPROC_ESP32C3_BEETLE
    #define COPROC_HW_CONFIG_ID "0x0B"
#elif defined(COPROC_ESP32C3_XIAO)
    #define COPROC_HW_CONFIG_ID "0x1B"
#elif defined(COPROC_ESP32C3_SUPER_MINI)
    #define COPROC_HW_CONFIG_ID "0x2B"
#endif

// Coprocessor UART Configuration
#ifdef COPROC_ESP32C3_BEETLE
    #define UART_TX_PIN 1
    #define UART_RX_PIN 0
    #define REBOOT_SIGNAL_PIN 7
#elif defined(COPROC_ESP32C3_XIAO)
    #define UART_TX_PIN 4
    #define UART_RX_PIN 3
    #define REBOOT_SIGNAL_PIN 5
#elif defined(COPROC_ESP32C3_SUPER_MINI)
    #define UART_TX_PIN 1
    #define UART_RX_PIN 0
    #define REBOOT_SIGNAL_PIN 3
#endif
