#include "McpMatrix.h"

#include <cstring>

bool McpMatrix::begin(String& error) {
    if (AppConfig::TCA9548A_RESET_PIN >= 0) {
        pinMode(AppConfig::TCA9548A_RESET_PIN, OUTPUT);
        digitalWrite(AppConfig::TCA9548A_RESET_PIN, HIGH);
    }

    Wire.begin(AppConfig::I2C_SDA_PIN, AppConfig::I2C_SCL_PIN);
    Wire.setClock(AppConfig::I2C_CLOCK_HZ);
    Wire.setTimeOut(AppConfig::I2C_TIMEOUT_MS);

    if (!probe(AppConfig::TCA9548A_ADDRESS)) {
        error = "PCA/TCA9548A not found at 0x70 on SDA=" +
                String(AppConfig::I2C_SDA_PIN) + " SCL=" + String(AppConfig::I2C_SCL_PIN);
        return false;
    }
    if (!resetMultiplexer(error)) {
        return false;
    }
    return initializeAll(error);
}

bool McpMatrix::resetMultiplexer(String& error) {
    if (AppConfig::TCA9548A_RESET_PIN >= 0) {
        digitalWrite(AppConfig::TCA9548A_RESET_PIN, LOW);
        delay(2);
        digitalWrite(AppConfig::TCA9548A_RESET_PIN, HIGH);
        delay(2);
    }
    selectedDirection_ = -1;
    return disableAllDirections(error);
}

bool McpMatrix::disableAllDirections(String& error) {
    Wire.beginTransmission(AppConfig::TCA9548A_ADDRESS);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) {
        error = "Failed to write 0x00 to PCA/TCA9548A at 0x70";
        return false;
    }
    selectedDirection_ = -1;
    return true;
}

bool McpMatrix::selectDirection(uint8_t direction, String& error) {
    if (direction >= AppConfig::DIRECTION_COUNT) {
        error = "Invalid I2C direction";
        return false;
    }
    if (selectedDirection_ == static_cast<int8_t>(direction)) {
        return true;
    }

    Wire.beginTransmission(AppConfig::TCA9548A_ADDRESS);
    Wire.write(static_cast<uint8_t>(1U << direction));
    if (Wire.endTransmission() != 0) {
        error = "Failed to switch PCA/TCA9548A to channel " + String(direction);
        selectedDirection_ = -1;
        return false;
    }
    selectedDirection_ = static_cast<int8_t>(direction);
    return true;
}

bool McpMatrix::probe(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

bool McpMatrix::writeRegister8(uint8_t address, uint8_t reg, uint8_t value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool McpMatrix::writeRegister16(uint8_t address, uint8_t reg, uint16_t value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(static_cast<uint8_t>(value & 0xFF));
    Wire.write(static_cast<uint8_t>((value >> 8) & 0xFF));
    return Wire.endTransmission() == 0;
}

bool McpMatrix::readRegister16(uint8_t address, uint8_t reg, uint16_t& value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    const size_t received = Wire.requestFrom(address, static_cast<uint8_t>(2), static_cast<uint8_t>(true));
    if (received != 2 || Wire.available() < 2) {
        return false;
    }

    const uint8_t low = Wire.read();
    const uint8_t high = Wire.read();
    value = static_cast<uint16_t>(low) | (static_cast<uint16_t>(high) << 8);
    return true;
}

bool McpMatrix::initializeAll(String& error) {
    for (uint8_t direction = 0; direction < AppConfig::DIRECTION_COUNT; ++direction) {
        if (!selectDirection(direction, error)) {
            return false;
        }

        for (uint8_t module = 0; module < AppConfig::MODULES_PER_DIRECTION; ++module) {
            const uint8_t address = static_cast<uint8_t>(0x20 + module);
            if (!probe(address)) {
                error = "MCP23017 not responding: direction " + String(direction) +
                        ", module " + String(module) + ", address 0x" + String(address, HEX);
                return false;
            }

            if (!writeRegister8(address, REG_IOCON, 0x00) ||
                !writeRegister16(address, REG_IPOLA, 0x0000) ||
                !writeRegister16(address, REG_GPINTENA, 0x0000) ||
                !writeRegister16(address, REG_OLATA, 0x0000) ||
                !writeRegister16(address, REG_IODIRA, 0xFFFF) ||
                !writeRegister16(address, REG_GPPUA, 0xFFFF)) {
                error = "MCP23017 init failed: direction " + String(direction) +
                        ", module " + String(module);
                return false;
            }
        }
    }
    return true;
}

bool McpMatrix::setAllInputs(String& error) {
    for (uint8_t direction = 0; direction < AppConfig::DIRECTION_COUNT; ++direction) {
        if (!selectDirection(direction, error)) {
            return false;
        }
        for (uint8_t module = 0; module < AppConfig::MODULES_PER_DIRECTION; ++module) {
            const uint8_t address = static_cast<uint8_t>(0x20 + module);
            if (!writeRegister16(address, REG_IODIRA, 0xFFFF) ||
                !writeRegister16(address, REG_GPPUA, 0xFFFF)) {
                error = "Failed to return all lines to input: D" + String(direction) +
                        " M" + String(module);
                return false;
            }
        }
    }
    return true;
}

bool McpMatrix::readAllLowMask(uint8_t* row, String& error) {
    if (row == nullptr) {
        error = "Null matrix row buffer";
        return false;
    }
    memset(row, 0, AppConfig::ROW_BYTES);

    for (uint8_t direction = 0; direction < AppConfig::DIRECTION_COUNT; ++direction) {
        if (!selectDirection(direction, error)) {
            return false;
        }
        for (uint8_t module = 0; module < AppConfig::MODULES_PER_DIRECTION; ++module) {
            const uint8_t address = static_cast<uint8_t>(0x20 + module);
            uint16_t gpio = 0;
            if (!readRegister16(address, REG_GPIOA, gpio)) {
                error = "GPIOA/GPIOB read failed: D" + String(direction) +
                        " M" + String(module);
                return false;
            }
            const uint16_t lowMask = static_cast<uint16_t>(~gpio);
            const size_t moduleIndex = direction * AppConfig::MODULES_PER_DIRECTION + module;
            const size_t offset = moduleIndex * 2;
            row[offset] = static_cast<uint8_t>(lowMask & 0xFF);
            row[offset + 1] = static_cast<uint8_t>((lowMask >> 8) & 0xFF);
        }
    }
    return true;
}

bool McpMatrix::drivePinLow(uint16_t globalPin, String& error) {
    if (globalPin >= AppConfig::PIN_COUNT) {
        error = "Invalid source pin number";
        return false;
    }

    const uint8_t direction = directionOf(globalPin);
    const uint8_t module = moduleOf(globalPin);
    const uint8_t pin = pinOf(globalPin);
    if (!selectDirection(direction, error)) {
        return false;
    }

    const uint8_t address = static_cast<uint8_t>(0x20 + module);
    const uint8_t directionRegister = pin < 8 ? REG_IODIRA : REG_IODIRB;
    const uint8_t bit = pin & 0x07;
    const uint8_t directionMask = static_cast<uint8_t>(0xFFU & ~(1U << bit));

    if (!writeRegister8(address, directionRegister, directionMask)) {
        error = "Failed to set OUTPUT LOW for " + pinName(globalPin);
        return false;
    }
    return true;
}

bool McpMatrix::releasePin(uint16_t globalPin, String& error) {
    if (globalPin >= AppConfig::PIN_COUNT) {
        error = "Invalid pin number to release";
        return false;
    }

    const uint8_t direction = directionOf(globalPin);
    const uint8_t module = moduleOf(globalPin);
    const uint8_t pin = pinOf(globalPin);
    if (!selectDirection(direction, error)) {
        return false;
    }

    const uint8_t address = static_cast<uint8_t>(0x20 + module);
    const uint8_t directionRegister = pin < 8 ? REG_IODIRA : REG_IODIRB;
    if (!writeRegister8(address, directionRegister, 0xFF)) {
        error = "Failed to return pin to input " + pinName(globalPin);
        return false;
    }
    return true;
}

bool McpMatrix::readPin(uint16_t globalPin, bool& high, String& error) {
    if (globalPin >= AppConfig::PIN_COUNT) {
        error = "Invalid pin number to read";
        return false;
    }

    const uint8_t direction = directionOf(globalPin);
    const uint8_t module = moduleOf(globalPin);
    const uint8_t pin = pinOf(globalPin);
    if (!selectDirection(direction, error)) {
        return false;
    }

    uint16_t gpio = 0;
    if (!readRegister16(static_cast<uint8_t>(0x20 + module), REG_GPIOA, gpio)) {
        error = "Readback failed for " + pinName(globalPin);
        return false;
    }
    high = (gpio & (1U << pin)) != 0;
    return true;
}

bool McpMatrix::waitPinHigh(uint16_t globalPin, uint32_t timeoutUs, String& error) {
    const uint32_t started = micros();
    while (static_cast<uint32_t>(micros() - started) < timeoutUs) {
        bool high = false;
        if (!readPin(globalPin, high, error)) {
            return false;
        }
        if (high) {
            return true;
        }
        delayMicroseconds(20);
    }
    error = "Pin did not return HIGH: " + pinName(globalPin);
    return false;
}

uint8_t McpMatrix::directionOf(uint16_t globalPin) {
    return static_cast<uint8_t>((globalPin / AppConfig::PINS_PER_MODULE) / AppConfig::MODULES_PER_DIRECTION);
}

uint8_t McpMatrix::moduleOf(uint16_t globalPin) {
    return static_cast<uint8_t>((globalPin / AppConfig::PINS_PER_MODULE) % AppConfig::MODULES_PER_DIRECTION);
}

uint8_t McpMatrix::pinOf(uint16_t globalPin) {
    return static_cast<uint8_t>(globalPin % AppConfig::PINS_PER_MODULE);
}

String McpMatrix::pinName(uint16_t globalPin) {
    const uint8_t direction = directionOf(globalPin);
    const uint8_t module = moduleOf(globalPin);
    const uint8_t pin = pinOf(globalPin);
    return "D" + String(direction) + "-M" + String(module) +
           "-GP" + (pin < 8 ? "A" : "B") + String(pin & 0x07) +
           " (#" + String(globalPin) + ")";
}
