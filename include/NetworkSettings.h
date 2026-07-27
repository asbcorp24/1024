#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "AppConfig.h"

struct NetworkConfig {
    bool dhcp = AppConfig::DEFAULT_USE_DHCP;
    IPAddress ip = AppConfig::DEFAULT_STATIC_IP;
    IPAddress gateway = AppConfig::DEFAULT_STATIC_GATEWAY;
    IPAddress subnet = AppConfig::DEFAULT_STATIC_SUBNET;
    IPAddress dns = AppConfig::DEFAULT_STATIC_DNS;
};

class NetworkSettings {
public:
    bool begin(String& error);
    NetworkConfig snapshot() const;
    bool save(const NetworkConfig& config, String& error);
    bool resetDefaults(String& error);
    bool ready() const;

    static bool validate(const NetworkConfig& config, String& error);
    static String ipToString(const IPAddress& address);

private:
    mutable SemaphoreHandle_t mutex_ = nullptr;
    Preferences preferences_;
    NetworkConfig config_;
    bool ready_ = false;

    static bool parseStoredIp(const String& value, IPAddress& address);
};
