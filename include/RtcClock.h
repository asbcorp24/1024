#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "BrowserTime.h"

class RtcClock {
public:
    enum class TimeSource : uint8_t {
        Unsynced,
        Ds3231,
        Browser
    };

    bool begin(String& error);
    bool syncFromBrowser(const BrowserTimeContext& browserTime, String& error);

    bool available() const;
    bool synced() const;
    TimeSource source() const;
    const BrowserTimeContext& lastSync() const;
    bool currentTime(BrowserTimeContext& value, String& error) const;

    String sourceName() const;

private:
    bool probeDevice();
    bool readRtcTime(BrowserTimeContext& result, String& error);
    bool writeRtcTime(const BrowserTimeContext& value, String& error);
    bool applySystemTime(const BrowserTimeContext& value, String& error);

    static uint8_t bcdToDec(uint8_t value);
    static uint8_t decToBcd(uint8_t value);

    bool available_ = false;
    bool synced_ = false;
    TimeSource source_ = TimeSource::Unsynced;
    BrowserTimeContext lastSync_;
};
