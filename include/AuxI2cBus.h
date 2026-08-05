#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>

namespace AuxI2cBus {

void begin();
bool lock(TickType_t timeout = portMAX_DELAY);
void unlock();
TwoWire& wire();

} // namespace AuxI2cBus
