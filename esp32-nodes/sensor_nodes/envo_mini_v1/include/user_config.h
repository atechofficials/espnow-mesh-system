#pragma once

/**
    * @file [user_config.h]
    * @brief Shared definitions for the ESP32 Sensor Node Firmware
    * @version 1.0.1
    * @author Mrinal (@atechofficials)
 */

// Sensor Node User Configurations
#define NODE_NAME       "BMP280-Node"   // change per node (max 24 chars)

// Dev Board Selection
#if !defined(FIREBEETLE_2_ESP32E) && !defined(ESP32C3_SUPER_MINI)
    #define FIREBEETLE_2_ESP32E
#endif

#if defined(FIREBEETLE_2_ESP32E) && defined(ESP32C3_SUPER_MINI)
    #error "Select only one Sensor Node dev board."
#endif

// BMP280 Wiring Configuration
#if !defined(BMP280_I2C_CONFIG) && !defined(BMP280_SPI_CONFIG)
    #define BMP280_SPI_CONFIG
#endif

#if defined(BMP280_I2C_CONFIG) && defined(BMP280_SPI_CONFIG)
    #error "Select only one BMP280 wiring configuration."
#endif

#ifdef FIREBEETLE_2_ESP32E
    #define DHT_PIN         16    // DHT22 data pin
    #define DHT_TYPE        DHT22
    #define TEMT6000_PIN    36    // TEMT6000 analog output (ADC1_CH0 - input only)
    #define PAIR_BTN_PIN    27    // Pairing button GPIO - active-LOW, uses internal pull-up
    #define LED_PIN         5     // WS2812B data pin
    #define LED_COUNT       1
    #ifdef BMP280_I2C_CONFIG
        #define BMP_I2C_SDA     21
        #define BMP_I2C_SCL     22
        #define BMP_ADDR_PRIM   0x76
        #define BMP_ADDR_SEC    0x77
        #define HW_CONFIG_ID "0x0B"
    #elif defined (BMP280_SPI_CONFIG)
        #define BMP_SCK      18
        #define BMP_MISO     19
        #define BMP_MOSI     23
        #define BMP_CS       17
        #define HW_CONFIG_ID "0x1B"
    #endif
#elif defined (ESP32C3_SUPER_MINI)
    #define DHT_PIN         1    // DHT22 data pin
    #define DHT_TYPE        DHT22
    #define TEMT6000_PIN    4    // TEMT6000 analog output (ADC1_CH4)
    #define PAIR_BTN_PIN    0    // Pairing button GPIO - active-LOW, uses internal pull-up
    #define LED_PIN         3     // WS2812B data pin
    #define LED_COUNT       1
    #ifdef BMP280_I2C_CONFIG
        #define BMP_I2C_SDA     7
        #define BMP_I2C_SCL     6
        #define BMP_ADDR_PRIM   0x76
        #define BMP_ADDR_SEC    0x77
        #define HW_CONFIG_ID "0x2B"
    #elif defined (BMP280_SPI_CONFIG)
        #define BMP_SCK      6
        #define BMP_MISO     5
        #define BMP_MOSI     7
        #define BMP_CS       10
        #define HW_CONFIG_ID "0x3B"
    #endif
#endif
