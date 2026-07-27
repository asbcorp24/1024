#include "BrowserTime.h"

#include <cstdlib>
#include <cstring>
#include <time.h>

namespace {

portMUX_TYPE timeMux = portMUX_INITIALIZER_UNLOCKED;
BrowserTimeContext currentTime;

bool parseUnsigned64(const String& value, uint64_t& result) {
    if (value.isEmpty()) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
    }

    char* end = nullptr;
    const unsigned long long parsed = strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') return false;
    result = static_cast<uint64_t>(parsed);
    return true;
}

bool parseSigned16(const String& value, int16_t& result) {
    if (value.isEmpty()) return false;
    char* end = nullptr;
    const long parsed = strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < -840 || parsed > 840) return false;
    result = static_cast<int16_t>(parsed);
    return true;
}

bool validTimeZone(const String& value) {
    if (value.isEmpty() || value.length() >= 64) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        const uint8_t c = static_cast<uint8_t>(value[i]);
        if (c < 0x20 || c == '"' || c == '\\') return false;
    }
    return true;
}

String formatIso(uint64_t epochMs, int16_t offsetMinutes, bool appendZulu) {
    const int64_t adjustedMs = static_cast<int64_t>(epochMs) +
                               static_cast<int64_t>(offsetMinutes) * 60000LL;
    if (adjustedMs < 0) return "";

    const time_t seconds = static_cast<time_t>(adjustedMs / 1000LL);
    const uint16_t milliseconds = static_cast<uint16_t>(adjustedMs % 1000LL);
    struct tm value {};
    if (gmtime_r(&seconds, &value) == nullptr) return "";

    char buffer[48];
    if (appendZulu) {
        snprintf(buffer,
                 sizeof(buffer),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
                 value.tm_year + 1900,
                 value.tm_mon + 1,
                 value.tm_mday,
                 value.tm_hour,
                 value.tm_min,
                 value.tm_sec,
                 milliseconds);
    } else {
        const char sign = offsetMinutes >= 0 ? '+' : '-';
        const int absoluteOffset = offsetMinutes >= 0 ? offsetMinutes : -offsetMinutes;
        snprintf(buffer,
                 sizeof(buffer),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03u%c%02d:%02d",
                 value.tm_year + 1900,
                 value.tm_mon + 1,
                 value.tm_mday,
                 value.tm_hour,
                 value.tm_min,
                 value.tm_sec,
                 milliseconds,
                 sign,
                 absoluteOffset / 60,
                 absoluteOffset % 60);
    }
    return String(buffer);
}

} // namespace

namespace BrowserTime {

bool setFromBrowser(const String& epochMs,
                    const String& utcOffsetMinutes,
                    const String& timeZone,
                    String& error) {
    BrowserTimeContext next;
    if (!parseUnsigned64(epochMs, next.startedAtEpochMs) || !next.valid()) {
        error = "Браузер передал недопустимую дату и время";
        return false;
    }
    if (!parseSigned16(utcOffsetMinutes, next.utcOffsetMinutes)) {
        error = "Браузер передал недопустимое смещение UTC";
        return false;
    }
    if (!validTimeZone(timeZone)) {
        error = "Браузер не передал корректный часовой пояс";
        return false;
    }

    strlcpy(next.timeZone, timeZone.c_str(), sizeof(next.timeZone));
    portENTER_CRITICAL(&timeMux);
    currentTime = next;
    portEXIT_CRITICAL(&timeMux);
    return true;
}

BrowserTimeContext snapshot() {
    BrowserTimeContext copy;
    portENTER_CRITICAL(&timeMux);
    copy = currentTime;
    portEXIT_CRITICAL(&timeMux);
    return copy;
}

String formatUtc(uint64_t epochMs) {
    return formatIso(epochMs, 0, true);
}

String formatLocal(uint64_t epochMs, int16_t utcOffsetMinutes) {
    return formatIso(epochMs, utcOffsetMinutes, false);
}

} // namespace BrowserTime
