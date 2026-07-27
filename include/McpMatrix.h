#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "AppConfig.h"

class McpMatrix {
public:
    bool begin(String& error);
    bool initializeAll(String& error);
    bool setAllInputs(String& error);
    bool readAllLowMask(uint8_t* row, String& error);
    bool drivePinLow(uint16_t globalPin, String& error);
    bool releasePin(uint16_t globalPin, String& error);
    bool waitPinHigh(uint16_t globalPin, uint32_t timeoutUs, String& error);
    bool readPin(uint16_t globalPin, bool& high, String& error);

    static uint8_t directionOf(uint16_t globalPin);
    static uint8_t moduleOf(uint16_t globalPin);
    static uint8_t pinOf(uint16_t globalPin);
    static String pinName(uint16_t globalPin);

private:
    static constexpr uint8_t REG_IODIRA = 0x00;
    static constexpr uint8_t REG_IODIRB = 0x01;
    static constexpr uint8_t REG_IPOLA = 0x02;
    static constexpr uint8_t REG_GPINTENA = 0x04;
    static constexpr uint8_t REG_IOCON = 0x0A;
    static constexpr uint8_t REG_GPPUA = 0x0C;
    static constexpr uint8_t REG_GPIOA = 0x12;
    static constexpr uint8_t REG_OLATA = 0x14;

    int8_t selectedDirection_ = -1;

    bool resetMultiplexer(String& error);
    bool disableAllDirections(String& error);
    bool selectDirection(uint8_t direction, String& error);
    bool probe(uint8_t address);
    bool writeRegister8(uint8_t address, uint8_t reg, uint8_t value);
    bool writeRegister16(uint8_t address, uint8_t reg, uint16_t value);
    bool readRegister16(uint8_t address, uint8_t reg, uint16_t& value);
};
