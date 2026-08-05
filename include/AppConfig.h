#pragma once

#include <Arduino.h>
#include <IPAddress.h>

namespace AppConfig {

constexpr uint8_t I2C_SDA_PIN = 8;
constexpr uint8_t I2C_SCL_PIN = 9;
constexpr uint32_t I2C_CLOCK_HZ = 400000;
constexpr uint32_t I2C_TIMEOUT_MS = 20;

constexpr uint8_t TCA9548A_ADDRESS = 0x70;
constexpr int8_t TCA9548A_RESET_PIN = -1;

// GM009605 / SSD1306 0.96" 128x64 работает на отдельном I2C-контроллере.
constexpr uint8_t OLED_SDA_PIN = 16;
constexpr uint8_t OLED_SCL_PIN = 17;
constexpr uint8_t OLED_I2C_ADDRESS = 0x3C;
constexpr uint32_t OLED_I2C_CLOCK_HZ = 400000;
constexpr uint16_t OLED_WIDTH = 128;
constexpr uint16_t OLED_HEIGHT = 64;

constexpr uint8_t RTC_SDA_PIN = 41;
constexpr uint8_t RTC_SCL_PIN = 42;
constexpr uint8_t RTC_I2C_ADDRESS = 0x68;
constexpr uint32_t RTC_I2C_CLOCK_HZ = 100000;
constexpr bool RTC_ENABLED = true;

// Механический энкодер с общей точкой GND и внутренними подтяжками ESP32.
constexpr uint8_t ENCODER_A_PIN = 18;
constexpr uint8_t ENCODER_B_PIN = 21;
constexpr uint8_t ENCODER_BUTTON_PIN = 47;
constexpr uint32_t ENCODER_BUTTON_DEBOUNCE_MS = 35;
constexpr int8_t SERVICE_UNLOCK_PIN = 40;
constexpr bool SERVICE_UNLOCK_ACTIVE_LOW = true;
#ifdef RGB_BUILTIN
constexpr int16_t SERVICE_UNLOCK_LED_PIN = RGB_BUILTIN;
#elif defined(LED_BUILTIN)
constexpr int16_t SERVICE_UNLOCK_LED_PIN = LED_BUILTIN;
#else
constexpr int16_t SERVICE_UNLOCK_LED_PIN = -1;
#endif
constexpr bool SERVICE_UNLOCK_LED_ACTIVE_HIGH = true;

constexpr uint8_t W5500_SCK_PIN = 12;
constexpr uint8_t W5500_MISO_PIN = 13;
constexpr uint8_t W5500_MOSI_PIN = 11;
constexpr uint8_t W5500_CS_PIN = 5;
constexpr int8_t W5500_RESET_PIN = 14;
constexpr uint8_t W5500_INT_PIN = 4;
constexpr uint8_t W5500_PHY_ADDRESS = 1;

constexpr const char* HOSTNAME = "cable-tester-1024";
constexpr const char* DEVICE_MODEL = "KSK-1024";
constexpr const char* FIRMWARE_VERSION = "2.1.0";
constexpr bool DEFAULT_USE_DHCP = true;
const IPAddress DEFAULT_STATIC_IP(192, 168, 1, 204);
const IPAddress DEFAULT_STATIC_DNS(192, 168, 1, 1);
const IPAddress DEFAULT_STATIC_GATEWAY(192, 168, 1, 1);
const IPAddress DEFAULT_STATIC_SUBNET(255, 255, 255, 0);

// Заполняются из NVS в setup() до WebApp::begin().
extern bool USE_DHCP;
extern IPAddress STATIC_IP;
extern IPAddress STATIC_DNS;
extern IPAddress STATIC_GATEWAY;
extern IPAddress STATIC_SUBNET;

constexpr uint16_t HTTP_PORT = 80;

constexpr size_t DIRECTION_COUNT = 8;
constexpr size_t MODULES_PER_DIRECTION = 8;
constexpr size_t PINS_PER_MODULE = 16;
constexpr size_t MODULE_COUNT = DIRECTION_COUNT * MODULES_PER_DIRECTION;
constexpr size_t PIN_COUNT = MODULE_COUNT * PINS_PER_MODULE;
constexpr size_t ROW_BYTES = PIN_COUNT / 8;
constexpr size_t MATRIX_BYTES = PIN_COUNT * ROW_BYTES;

constexpr uint32_t SOURCE_SETTLE_US = 80;
constexpr uint32_t RELEASE_TIMEOUT_US = 5000;
constexpr uint8_t UNSTABLE_RETRY_COUNT = 3;

constexpr const char* STATIC_FS_BASE = "/littlefs";
constexpr const char* DATA_FS_BASE = "/ffat";
constexpr const char* REFERENCE_DIR = "/references";
constexpr const char* RESULT_DIR = "/results";
constexpr size_t MAX_CALCULATION_JSON_BYTES = 1024U * 1024U;

constexpr uint32_t REFERENCE_MAGIC = 0x31464243UL;
constexpr uint16_t FILE_FORMAT_VERSION = 3;
constexpr bool UI_ONLY_MODE = false;
constexpr bool OLED_I2C_SCAN_ON_BOOT = true;
constexpr uint8_t OLED_I2C_SCAN_FIRST_ADDRESS = 0x08;
constexpr uint8_t OLED_I2C_SCAN_LAST_ADDRESS = 0x77;
constexpr bool OLED_DIAGNOSTIC_DISABLE_UPDATES = false;
constexpr bool HTTP_DEBUG_LOG = true;

} // namespace AppConfig
