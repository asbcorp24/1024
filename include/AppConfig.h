#pragma once

#include <Arduino.h>
#include <IPAddress.h>

namespace AppConfig {

constexpr uint8_t I2C_SDA_PIN = 8;
constexpr uint8_t I2C_SCL_PIN = 9;
constexpr uint32_t I2C_CLOCK_HZ = 400000;
constexpr uint32_t I2C_TIMEOUT_MS = 20;

constexpr uint8_t TCA9548A_ADDRESS = 0x70;
constexpr int8_t TCA9548A_RESET_PIN = 4;

constexpr uint8_t W5500_SCK_PIN = 12;
constexpr uint8_t W5500_MISO_PIN = 13;
constexpr uint8_t W5500_MOSI_PIN = 11;
constexpr uint8_t W5500_CS_PIN = 10;
constexpr int8_t W5500_RESET_PIN = 14;
constexpr uint8_t W5500_INT_PIN = 15;
constexpr uint8_t W5500_PHY_ADDRESS = 1;

constexpr const char* HOSTNAME = "cable-tester-1024";
constexpr bool USE_DHCP = true;
const IPAddress STATIC_IP(192, 168, 1, 204);
const IPAddress STATIC_DNS(192, 168, 1, 1);
const IPAddress STATIC_GATEWAY(192, 168, 1, 1);
const IPAddress STATIC_SUBNET(255, 255, 255, 0);
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
constexpr uint16_t FILE_FORMAT_VERSION = 1;

} // namespace AppConfig
