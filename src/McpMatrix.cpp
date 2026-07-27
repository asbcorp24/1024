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

    if (!resetMultiplexer(error)) {
        return false;
    }
    if (!probe(AppConfig::TCA9548A_ADDRESS)) {
        error = "TCA9548A не отвечает по адресу 0x70";
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
        error = "Не удалось отключить каналы TCA9548A";
        return false;
    }
    selectedDirection_ = -1;
    return true;
}

bool McpMatrix::selectDirection(uint8_t direction, String& error) {
    if (direction >= AppConfig::DIRECTION_COUNT) {
        error = "Недопустимое направление I2C";
        return false;
    }
    if (selectedDirection_ == static_cast<int8_t>(direction)) {
        return true;
    }

    Wire.beginTransmission(AppConfig::TCA9548A_ADDRESS);
    Wire.write(static_cast<uint8_t>(1U << direction));
    if (Wire.endTransmission() != 0) {
        error = "Ошибка переключения TCA9548A на направление " + String(direction);
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
                error = "MCP23017 не отвечает: направление " + String(direction) +
                        ", модуль " + String(module) + ", адрес 0x" + String(address, HEX);
                return false;
            }

            if (!writeRegister8(address, REG_IOCON, 0x00) ||
                !writeRegister16(address, REG_IPOLA, 0x0000) ||
                !writeRegister16(address, REG_GPINTENA, 0x0000) ||
                !writeRegister16(address, REG_OLATA, 0x0000) ||
                !writeRegister16(address, REG_IODIRA, 0xFFFF) ||
                !writeRegister16(address, REG_GPPUA, 0xFFFF)) {
                error = "Ошибка настройки MCP23017: направление " + String(direction) +
                        ", модуль " + String(module);
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
                error = "Не удалось перевести все линии во вход: D" + String(direction) +
                        " M" + String(module);
                return false;
            }
        }
    }
    return true;
}

bool McpMatrix::readAllLowMask(uint8_t* row, String& error) {
    if (row == nullptr) {
        error = "Нулевой буфер строки матрицы";
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
                error = "Ошибка чтения GPIOA/GPIOB: D" + String(direction) +
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
        error = "Недопустимый номер тестового пина";
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
        error = "Не удалось установить OUTPUT LOW для " + pinName(globalPin);
        return false;
    }
    return true;
}

bool McpMatrix::releasePin(uint16_t globalPin, String& error) {
    if (globalPin >= AppConfig::PIN_COUNT) {
        error = "Недопустимый номер освобождаемого пина";
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
        error = "Не удалось вернуть во вход " + pinName(globalPin);
        return false;
    }
    return true;
}

bool McpMatrix::readPin(uint16_t globalPin, bool& high, String& error) {
    if (globalPin >= AppConfig::PIN_COUNT) {
        error = "Недопустимый номер читаемого пина";
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
        error = "Ошибка контрольного чтения " + pinName(globalPin);
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
    error = "Линия не восстановилась после отпускания: " + pinName(globalPin);
    return false;
}

uint8_t McpMatrix::directionOf(uint16_t globalPin) {
    return static_cast<uint8_t>((globalPin >> 7) & 0x07);
}

uint8_t McpMatrix::moduleOf(uint16_t globalPin) {
    return static_cast<uint8_t>((globalPin >> 4) & 0x07);
}

uint8_t McpMatrix::pinOf(uint16_t globalPin) {
    return static_cast<uint8_t>(globalPin & 0x0F);
}

String McpMatrix::pinName(uint16_t globalPin) {
    const uint8_t pin = pinOf(globalPin);
    String result = "D" + String(directionOf(globalPin));
    result += "-M" + String(moduleOf(globalPin));
    result += pin < 8 ? "-GPA" : "-GPB";
    result += String(pin & 0x07);
    result += " (#" + String(globalPin) + ")";
    return result;
}
