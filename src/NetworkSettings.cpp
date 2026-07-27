#include "NetworkSettings.h"

namespace {

constexpr const char* NVS_NAMESPACE = "network";

bool nonZeroAddress(const IPAddress& address) {
    return address[0] != 0 || address[1] != 0 || address[2] != 0 || address[3] != 0;
}

} // namespace

bool NetworkSettings::begin(String& error) {
    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutex();
        if (mutex_ == nullptr) {
            error = "Не удалось создать mutex сетевых настроек";
            return false;
        }
    }

    if (!preferences_.begin(NVS_NAMESPACE, false)) {
        error = "Не удалось открыть NVS сетевых настроек";
        return false;
    }

    NetworkConfig loaded;
    loaded.dhcp = preferences_.getBool("dhcp", AppConfig::DEFAULT_USE_DHCP);

    IPAddress parsed;
    if (parseStoredIp(preferences_.getString("ip", NetworkSettings::ipToString(AppConfig::DEFAULT_STATIC_IP)), parsed)) {
        loaded.ip = parsed;
    }
    if (parseStoredIp(preferences_.getString("gateway", NetworkSettings::ipToString(AppConfig::DEFAULT_STATIC_GATEWAY)), parsed)) {
        loaded.gateway = parsed;
    }
    if (parseStoredIp(preferences_.getString("subnet", NetworkSettings::ipToString(AppConfig::DEFAULT_STATIC_SUBNET)), parsed)) {
        loaded.subnet = parsed;
    }
    if (parseStoredIp(preferences_.getString("dns", NetworkSettings::ipToString(AppConfig::DEFAULT_STATIC_DNS)), parsed)) {
        loaded.dns = parsed;
    }

    if (!validate(loaded, error)) {
        loaded = NetworkConfig{};
        error = "В NVS были некорректные сетевые параметры; применены значения по умолчанию";
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    config_ = loaded;
    ready_ = true;
    xSemaphoreGive(mutex_);
    return true;
}

NetworkConfig NetworkSettings::snapshot() const {
    NetworkConfig copy;
    if (mutex_ == nullptr) return copy;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    copy = config_;
    xSemaphoreGive(mutex_);
    return copy;
}

bool NetworkSettings::save(const NetworkConfig& config, String& error) {
    if (!ready_ || mutex_ == nullptr) {
        error = "NVS сетевых настроек не готово";
        return false;
    }
    if (!validate(config, error)) return false;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool ok = preferences_.putBool("dhcp", config.dhcp) == 1 &&
                    preferences_.putString("ip", ipToString(config.ip)) > 0 &&
                    preferences_.putString("gateway", ipToString(config.gateway)) > 0 &&
                    preferences_.putString("subnet", ipToString(config.subnet)) > 0 &&
                    preferences_.putString("dns", ipToString(config.dns)) > 0;
    if (ok) config_ = config;
    xSemaphoreGive(mutex_);

    if (!ok) {
        error = "Не удалось полностью записать сетевые параметры в NVS";
        return false;
    }
    return true;
}

bool NetworkSettings::resetDefaults(String& error) {
    return save(NetworkConfig{}, error);
}

bool NetworkSettings::ready() const {
    if (mutex_ == nullptr) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool value = ready_;
    xSemaphoreGive(mutex_);
    return value;
}

bool NetworkSettings::validate(const NetworkConfig& config, String& error) {
    if (!config.dhcp) {
        if (!nonZeroAddress(config.ip)) {
            error = "Статический IP не может быть 0.0.0.0";
            return false;
        }
        if (!nonZeroAddress(config.subnet)) {
            error = "Маска сети не может быть 0.0.0.0";
            return false;
        }
        if (!nonZeroAddress(config.gateway)) {
            error = "Шлюз не может быть 0.0.0.0";
            return false;
        }
    }
    return true;
}

String NetworkSettings::ipToString(const IPAddress& address) {
    return address.toString();
}

bool NetworkSettings::parseStoredIp(const String& value, IPAddress& address) {
    IPAddress parsed;
    if (!parsed.fromString(value)) return false;
    address = parsed;
    return true;
}
