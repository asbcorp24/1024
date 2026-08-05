#include "RtcClock.h"

#include <esp32-hal-log.h>
#include <sys/time.h>
#include <time.h>

#include "AppConfig.h"

namespace {

constexpr uint32_t SOFT_I2C_HALF_PERIOD_US = 5;

void lineRelease(uint8_t pin) {
    pinMode(pin, INPUT_PULLUP);
}

void lineLow(uint8_t pin) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
}

bool lineRead(uint8_t pin) {
    pinMode(pin, INPUT_PULLUP);
    return digitalRead(pin) != LOW;
}

void i2cDelay() {
    delayMicroseconds(SOFT_I2C_HALF_PERIOD_US);
}

bool softI2cStart() {
    lineRelease(AppConfig::RTC_SDA_PIN);
    lineRelease(AppConfig::RTC_SCL_PIN);
    i2cDelay();
    if (!lineRead(AppConfig::RTC_SDA_PIN) || !lineRead(AppConfig::RTC_SCL_PIN)) return false;
    lineLow(AppConfig::RTC_SDA_PIN);
    i2cDelay();
    lineLow(AppConfig::RTC_SCL_PIN);
    i2cDelay();
    return true;
}

void softI2cStop() {
    lineLow(AppConfig::RTC_SDA_PIN);
    i2cDelay();
    lineRelease(AppConfig::RTC_SCL_PIN);
    i2cDelay();
    lineRelease(AppConfig::RTC_SDA_PIN);
    i2cDelay();
}

bool softI2cWriteBit(bool high) {
    if (high) lineRelease(AppConfig::RTC_SDA_PIN);
    else lineLow(AppConfig::RTC_SDA_PIN);
    i2cDelay();
    lineRelease(AppConfig::RTC_SCL_PIN);
    i2cDelay();
    const bool ok = lineRead(AppConfig::RTC_SCL_PIN);
    lineLow(AppConfig::RTC_SCL_PIN);
    i2cDelay();
    return ok;
}

bool softI2cReadBit() {
    lineRelease(AppConfig::RTC_SDA_PIN);
    i2cDelay();
    lineRelease(AppConfig::RTC_SCL_PIN);
    i2cDelay();
    const bool bit = lineRead(AppConfig::RTC_SDA_PIN);
    lineLow(AppConfig::RTC_SCL_PIN);
    i2cDelay();
    return bit;
}

bool softI2cWriteByte(uint8_t value, bool& ack) {
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if (!softI2cWriteBit((value & 0x80U) != 0)) return false;
        value <<= 1;
    }
    ack = !softI2cReadBit();
    return true;
}

bool softI2cReadByte(uint8_t& value, bool ack) {
    uint8_t result = 0;
    for (uint8_t bit = 0; bit < 8; ++bit) {
        result = static_cast<uint8_t>((result << 1) | (softI2cReadBit() ? 1U : 0U));
    }
    if (!softI2cWriteBit(!ack)) return false;
    value = result;
    return true;
}

uint64_t epochMsFromTm(const tm& value) {
    tm local = value;
    const time_t seconds = mktime(&local);
    if (seconds < 0) return 0;
    return static_cast<uint64_t>(seconds) * 1000ULL;
}

} // namespace

bool RtcClock::begin(String& error) {
    if (!AppConfig::RTC_ENABLED) {
        error = "DS3231 disabled in firmware";
        available_ = false;
        synced_ = false;
        source_ = TimeSource::Unsynced;
        return false;
    }

    lineRelease(AppConfig::RTC_SDA_PIN);
    lineRelease(AppConfig::RTC_SCL_PIN);

    if (!probeDevice()) {
        error = "DS3231 not found on software I2C";
        available_ = false;
        synced_ = false;
        source_ = TimeSource::Unsynced;
        return false;
    }

    available_ = true;

    BrowserTimeContext rtcTime;
    if (!readRtcTime(rtcTime, error)) {
        synced_ = false;
        source_ = TimeSource::Unsynced;
        return false;
    }

    if (!applySystemTime(rtcTime, error)) {
        synced_ = false;
        source_ = TimeSource::Unsynced;
        return false;
    }

    lastSync_ = rtcTime;
    synced_ = true;
    source_ = TimeSource::Ds3231;
    log_i("DS3231 time applied: %s", BrowserTime::formatUtc(lastSync_.startedAtEpochMs).c_str());
    return true;
}

bool RtcClock::syncFromBrowser(const BrowserTimeContext& browserTime, String& error) {
    if (!browserTime.valid()) {
        error = "Invalid browser time";
        return false;
    }
    if (!applySystemTime(browserTime, error)) return false;
    if (!AppConfig::RTC_ENABLED) {
        lastSync_ = browserTime;
        synced_ = true;
        source_ = TimeSource::Browser;
        log_i("System time synced from browser without DS3231");
        return true;
    }
    if (!available_) {
        error = "DS3231 is not available on software I2C";
        return false;
    }
    if (!writeRtcTime(browserTime, error)) return false;

    lastSync_ = browserTime;
    synced_ = true;
    source_ = TimeSource::Browser;
    log_i("DS3231 synced from browser: %s", BrowserTime::formatUtc(lastSync_.startedAtEpochMs).c_str());
    return true;
}

bool RtcClock::available() const {
    return available_;
}

bool RtcClock::synced() const {
    return synced_;
}

RtcClock::TimeSource RtcClock::source() const {
    return source_;
}

const BrowserTimeContext& RtcClock::lastSync() const {
    return lastSync_;
}

bool RtcClock::currentTime(BrowserTimeContext& value, String& error) const {
    if (!synced_) {
        error = "RTC/system time is not synchronized";
        return false;
    }

    struct timeval now {};
    if (gettimeofday(&now, nullptr) != 0) {
        error = "Could not read current system time";
        return false;
    }

    BrowserTimeContext current = lastSync_;
    current.startedAtEpochMs =
        static_cast<uint64_t>(now.tv_sec) * 1000ULL +
        static_cast<uint64_t>(now.tv_usec / 1000ULL);
    value = current;
    return true;
}

String RtcClock::sourceName() const {
    switch (source_) {
        case TimeSource::Ds3231: return "ds3231";
        case TimeSource::Browser: return "browser";
        default: return "unsynced";
    }
}

bool RtcClock::probeDevice() {
    bool ack = false;
    if (!softI2cStart()) return false;
    const bool ok = softI2cWriteByte(static_cast<uint8_t>((AppConfig::RTC_I2C_ADDRESS << 1) | 0U), ack) && ack;
    softI2cStop();
    return ok;
}

bool RtcClock::readRtcTime(BrowserTimeContext& result, String& error) {
    bool ack = false;
    if (!softI2cStart()) {
        error = "DS3231 start condition failed";
        return false;
    }
    if (!softI2cWriteByte(static_cast<uint8_t>((AppConfig::RTC_I2C_ADDRESS << 1) | 0U), ack) || !ack) {
        softI2cStop();
        error = "DS3231 register select failed";
        return false;
    }
    if (!softI2cWriteByte(0x00, ack) || !ack) {
        softI2cStop();
        error = "DS3231 register pointer write failed";
        return false;
    }
    if (!softI2cStart()) {
        error = "DS3231 repeated start failed";
        return false;
    }
    if (!softI2cWriteByte(static_cast<uint8_t>((AppConfig::RTC_I2C_ADDRESS << 1) | 1U), ack) || !ack) {
        softI2cStop();
        error = "DS3231 read address failed";
        return false;
    }

    uint8_t raw[7] = {};
    for (uint8_t i = 0; i < 7; ++i) {
        if (!softI2cReadByte(raw[i], i < 6)) {
            softI2cStop();
            error = "DS3231 returned incomplete time frame";
            return false;
        }
    }
    softI2cStop();

    const uint8_t seconds = bcdToDec(raw[0] & 0x7F);
    const uint8_t minutes = bcdToDec(raw[1] & 0x7F);
    const uint8_t hours = bcdToDec(raw[2] & 0x3F);
    const uint8_t day = bcdToDec(raw[4] & 0x3F);
    const uint8_t month = bcdToDec(raw[5] & 0x1F);
    const uint16_t year = static_cast<uint16_t>(2000 + bcdToDec(raw[6]));

    tm value {};
    value.tm_year = static_cast<int>(year) - 1900;
    value.tm_mon = static_cast<int>(month) - 1;
    value.tm_mday = day;
    value.tm_hour = hours;
    value.tm_min = minutes;
    value.tm_sec = seconds;
    value.tm_isdst = -1;

    const uint64_t epochMs = epochMsFromTm(value);
    if (epochMs < 946684800000ULL || epochMs > 4102444800000ULL) {
        error = "DS3231 returned out-of-range date/time";
        return false;
    }

    result.startedAtEpochMs = epochMs;
    result.utcOffsetMinutes = 0;
    strlcpy(result.timeZone, "RTC/DS3231", sizeof(result.timeZone));
    return true;
}

bool RtcClock::writeRtcTime(const BrowserTimeContext& value, String& error) {
    const time_t seconds = static_cast<time_t>(value.startedAtEpochMs / 1000ULL);
    tm utc {};
    if (gmtime_r(&seconds, &utc) == nullptr) {
        error = "Unable to convert browser time for DS3231";
        return false;
    }

    bool ack = false;
    if (!softI2cStart()) {
        error = "DS3231 start condition failed";
        return false;
    }
    if (!softI2cWriteByte(static_cast<uint8_t>((AppConfig::RTC_I2C_ADDRESS << 1) | 0U), ack) || !ack) {
        softI2cStop();
        error = "DS3231 write address failed";
        return false;
    }

    const uint8_t payload[8] = {
        0x00,
        decToBcd(static_cast<uint8_t>(utc.tm_sec)),
        decToBcd(static_cast<uint8_t>(utc.tm_min)),
        decToBcd(static_cast<uint8_t>(utc.tm_hour)),
        decToBcd(static_cast<uint8_t>(utc.tm_wday == 0 ? 7 : utc.tm_wday)),
        decToBcd(static_cast<uint8_t>(utc.tm_mday)),
        decToBcd(static_cast<uint8_t>(utc.tm_mon + 1)),
        decToBcd(static_cast<uint8_t>((utc.tm_year + 1900) - 2000))
    };
    for (uint8_t i = 0; i < sizeof(payload); ++i) {
        if (!softI2cWriteByte(payload[i], ack) || !ack) {
            softI2cStop();
            error = "DS3231 write failed";
            return false;
        }
    }
    softI2cStop();
    if (!probeDevice()) {
        error = "DS3231 write failed";
        return false;
    }
    return true;
}

bool RtcClock::applySystemTime(const BrowserTimeContext& value, String& error) {
    timeval tv {};
    tv.tv_sec = static_cast<time_t>(value.startedAtEpochMs / 1000ULL);
    tv.tv_usec = static_cast<suseconds_t>((value.startedAtEpochMs % 1000ULL) * 1000ULL);
    if (settimeofday(&tv, nullptr) != 0) {
        error = "Unable to set ESP32 system time";
        return false;
    }
    return true;
}

uint8_t RtcClock::bcdToDec(uint8_t value) {
    return static_cast<uint8_t>((value >> 4) * 10U + (value & 0x0F));
}

uint8_t RtcClock::decToBcd(uint8_t value) {
    return static_cast<uint8_t>(((value / 10U) << 4) | (value % 10U));
}
