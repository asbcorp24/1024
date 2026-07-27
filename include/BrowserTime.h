#pragma once

#include <Arduino.h>

struct BrowserTimeContext {
    uint64_t startedAtEpochMs = 0;
    int16_t utcOffsetMinutes = 0;
    char timeZone[64] = {};

    bool valid() const {
        return startedAtEpochMs >= 946684800000ULL &&
               startedAtEpochMs <= 4102444800000ULL;
    }
};

namespace BrowserTime {

bool parseFromBrowser(const String& epochMs,
                      const String& utcOffsetMinutes,
                      const String& timeZone,
                      BrowserTimeContext& context,
                      String& error);

String formatUtc(uint64_t epochMs);
String formatLocal(uint64_t epochMs, int16_t utcOffsetMinutes);

} // namespace BrowserTime
