#pragma once

/**
    * @file [user_config.h]
    * @brief Shared definitions for the ESP32 Hybrid Relay Node Firmware
    * @version 1.0.0
    * @author Mrinal (@atechofficials)
 */

// Hybrid Node User Configurations
#define NODE_NAME       "RFID-Hybrid-Node"   // change per node (max 24 chars)

// Dev Board Selection
#define ESP32_DEVKITC
// #define ESP32C3_SUPER_MINI
// #define ESP32C6_WROOM_1

#ifdef ESP32_DEVKITC
    #define HW_CONFIG_ID "0x0D"
    #define RELAY1_PIN      26    // Active LOW relay control pins - adjust as needed for your hardware setup
    #define RELAY2_PIN      27
    #define RELAY3_PIN      32
    #define RELAY4_PIN      33
    #define TOUCH1_PIN      25    // TTP224 touch sensor 1 (channels 1-4) - active HIGH, with external pull-downs to GND. Adjust pin numbers as needed for your hardware setup.
    #define TOUCH2_PIN      4     // TTP224 touch sensor 2
    #define TOUCH3_PIN      13    // TTP224 touch sensor 3
    #define TOUCH4_PIN      14    // TTP224 touch sensor 4
    #define PAIR_BTN_PIN    16    // Pairing button GPIO pin (active-LOW, uses internal pull-up)
    #define LED_PIN         22     // WS2812B data pin
    #define LED_COUNT       1     
    #define RELAY_COUNT     4
    #define TOUCH_DEBOUNCE_MS 35
    #define SPI_CLK 18
    #define SPI_MOSI 23
    #define SPI_MISO 19
    #define RFID_CS_PIN 17
    #define RFID_RST_PIN 21
    #define RFID_INIT_RETRY_MS 5000UL
    #define RFID_HEALTH_CHECK_MS 5000UL
    #define RFID_READ_FAIL_RESET_THRESHOLD 3
    bool relay_active_high = false; // Set to true if your relay module is active HIGH, false if active LOW
#endif
