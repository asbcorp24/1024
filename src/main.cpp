#include <Arduino.h>

#include "AppConfig.h"
#include "DisplayMenu.h"
#include "McpMatrix.h"
#include "NetworkSettings.h"
#include "RtcClock.h"
#include "StorageManager.h"
#include "TestEngine.h"
#include "WebApp.h"

StorageManager storage;
McpMatrix matrix;
TestEngine engine(matrix, storage);
NetworkSettings networkSettings;
RtcClock rtc;
WebApp web(storage, engine, rtc);
DisplayMenu displayMenu(networkSettings, engine, web);

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("1024 Cable Assembly Tester / ESP32-S3 N16R8");
    String networkSettingsError;
    const bool networkSettingsReady = networkSettings.begin(networkSettingsError);
    if (!networkSettingsReady) {
        Serial.println("Network settings error: " + networkSettingsError);
    } else {
        if (!networkSettingsError.isEmpty()) Serial.println(networkSettingsError);
        const NetworkConfig config = networkSettings.snapshot();
        AppConfig::USE_DHCP = config.dhcp;
        AppConfig::STATIC_IP = config.ip;
        AppConfig::STATIC_GATEWAY = config.gateway;
        AppConfig::STATIC_SUBNET = config.subnet;
        AppConfig::STATIC_DNS = config.dns;
        Serial.println(String("Network mode: ") + (config.dhcp ? "DHCP" : "STATIC"));
        if (!config.dhcp) {
            Serial.println("Static IP: " + config.ip.toString());
            Serial.println("Gateway: " + config.gateway.toString());
            Serial.println("Subnet: " + config.subnet.toString());
            Serial.println("DNS: " + config.dns.toString());
        }
    }

    String storageError;
    const bool storageReady = storage.begin(storageError);
    if (!storageReady) {
        Serial.println("Storage error: " + storageError);
    } else {
        Serial.println("LittleFS + FFat mounted");
    }

    String matrixError;
    bool matrixReady = false;
    if (AppConfig::UI_ONLY_MODE) {
        matrixError = "UI-only mode: TCA9548A/MCP23017 disabled";
        Serial.println(matrixError);
        log_w("BOOT: matrix disabled by UI_ONLY_MODE");
    } else {
        matrixReady = matrix.begin(matrixError);
        if (!matrixReady) {
            Serial.println("MCP matrix error: " + matrixError);
            log_e("BOOT: McpMatrix.begin failed: %s", matrixError.c_str());
        } else {
            Serial.println("64 MCP23017 initialized");
            log_i("BOOT: McpMatrix.begin ok");
        }
    }

    const bool hardwareReady = storageReady && (AppConfig::UI_ONLY_MODE || matrixReady);
    engine.begin(hardwareReady,
                 storageReady
                     ? (AppConfig::UI_ONLY_MODE ? String("UI-only mode: matrix disabled") : matrixError)
                     : storageError);
    log_i("BOOT: TestEngine.begin hardwareReady=%s", hardwareReady ? "true" : "false");

    String rtcError;
    if (!rtc.begin(rtcError)) {
        Serial.println("RTC warning: " + rtcError);
    } else {
        Serial.println("DS3231 ready on software I2C");
    }

    String networkError;
    if (!web.begin(networkError)) {
        Serial.println("Ethernet/web error: " + networkError);
    } else {
        Serial.println("Async HTTP server started; IP will be reported by ETH event");
    }

    String displayError;
    if (!networkSettingsReady) {
        Serial.println("OLED menu disabled because NVS is unavailable");
    } else if (!displayMenu.begin(displayError)) {
        Serial.println("OLED/menu error: " + displayError);
    } else {
        Serial.println("Async GM009605 OLED menu started");
    }
}

void loop() {
    // HTTP, SSE, OLED-меню, энкодер, загрузка файлов и измерения работают в отдельных задачах.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
