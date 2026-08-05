#include "AuxI2cBus.h"

#include <freertos/semphr.h>

#include "AppConfig.h"

namespace {

SemaphoreHandle_t busMutex = nullptr;
bool busStarted = false;

} // namespace

namespace AuxI2cBus {

void begin() {
    if (busMutex == nullptr) busMutex = xSemaphoreCreateMutex();
    if (busStarted) return;

    Wire1.begin(AppConfig::OLED_SDA_PIN,
                AppConfig::OLED_SCL_PIN,
                AppConfig::RTC_I2C_CLOCK_HZ);
    busStarted = true;
}

bool lock(TickType_t timeout) {
    begin();
    return busMutex != nullptr && xSemaphoreTake(busMutex, timeout) == pdTRUE;
}

void unlock() {
    if (busMutex != nullptr) xSemaphoreGive(busMutex);
}

TwoWire& wire() {
    return Wire1;
}

} // namespace AuxI2cBus
