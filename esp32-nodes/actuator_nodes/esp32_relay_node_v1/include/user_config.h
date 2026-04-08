#pragma once

/**
    * @file [user_config.h]
    * @brief Shared definitions for the ESP32 Actuator Relay Node Firmware
    * @version 1.0.1
    * @author Mrinal (@atechofficials)
 */

// Actuator Node User Configurations
#define NODE_NAME       "Relay-Node"   // change per node (max 24 chars)

// Dev Board Selection
#if !defined(ESP32_DEVKITC) && !defined(ESP32C3_WROOM) && !defined(ESP32C6_WROOM)
    #define ESP32_DEVKITC
#endif

#if defined(ESP32_DEVKITC) && defined(ESP32C3_WROOM) && defined(ESP32C6_WROOM)
    #error "Select only one Actuator Relay Node dev board."
#endif

#ifdef ESP32_DEVKITC
    #define HW_CONFIG_ID "0x0C"
    #define RELAY_COUNT     4
    #define RELAY1_PIN      26    // Active LOW relay control pins - adjust as needed for your hardware setup
    #define RELAY2_PIN      27
    #define RELAY3_PIN      32
    #define RELAY4_PIN      33
    #define TOUCH1_PIN      25    // TTP224 touch sensor 1 (channels 1-4) - active HIGH, with external pull-downs to GND. Adjust pin numbers as needed for your hardware setup.
    #define TOUCH2_PIN      4     // TTP224 touch sensor 2
    #define TOUCH3_PIN      13    // TTP224 touch sensor 3
    #define TOUCH4_PIN      14    // TTP224 touch sensor 4
    #define PAIR_BTN_PIN    16    // Pairing button GPIO pin (active-LOW, uses internal pull-up)
    #define LED_PIN         22    // WS2812B data pin
    #define LED_COUNT       1     // Number of WS2812B LEDs on board
    #define TOUCH_DEBOUNCE_MS 35
    bool relay_active_high = false; // Set to true if your relay module is active HIGH, false if active LOW
#elif defined(ESP32C3_WROOM)
    #define HW_CONFIG_ID "0x1C"
    #define RELAY_COUNT     4
    #define RELAY1_PIN      3     // Active LOW relay control pins - adjust as needed for your hardware setup
    #define RELAY2_PIN      4
    #define RELAY3_PIN      5
    #define RELAY4_PIN      6
    #define TOUCH1_PIN      0     // TTP224 touch sensor 1 (channels 1-4) - active HIGH, with external pull-downs to GND. Adjust pin numbers as needed for your hardware setup.
    #define TOUCH2_PIN      1     // TTP224 touch sensor 2
    #define TOUCH3_PIN      7     // TTP224 touch sensor 3
    #define TOUCH4_PIN      10    // TTP224 touch sensor 4
    #define PAIR_BTN_PIN    19    // Pairing button GPIO pin (active-LOW, uses internal pull-up)
    #define LED_PIN         18    // WS2812B data pin
    #define LED_COUNT       1     // Number of WS2812B LEDs on board
    #define TOUCH_DEBOUNCE_MS 35
    bool relay_active_high = false; // Set to true if your relay module is active HIGH, false if active LOW
#elif defined(ESP32C6_WROOM)
    #define HW_CONFIG_ID "0x2C"
    #define RELAY_COUNT     4
    #define RELAY1_PIN      6     // Active LOW relay control pins - adjust as needed for your hardware setup
    #define RELAY2_PIN      7
    #define RELAY3_PIN      10
    #define RELAY4_PIN      11
    #define TOUCH1_PIN      0     // TTP224 touch sensor 1 (channels 1-4) - active HIGH, with external pull-downs to GND. Adjust pin numbers as needed for your hardware setup.
    #define TOUCH2_PIN      1     // TTP224 touch sensor 2
    #define TOUCH3_PIN      2     // TTP224 touch sensor 3
    #define TOUCH4_PIN      3     // TTP224 touch sensor 4
    #define PAIR_BTN_PIN    18    // Pairing button GPIO pin (active-LOW, uses internal pull-up)
    #define LED_PIN         14    // WS2812B data pin
    #define LED_COUNT       1     // Number of WS2812B LEDs on board
    #define TOUCH_DEBOUNCE_MS 35
    bool relay_active_high = false; // Set to true if your relay module is active HIGH, false if active LOW
#endif