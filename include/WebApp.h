#pragma once

#include <Arduino.h>
#include <Ethernet.h>
#include <SPI.h>

#include "StorageManager.h"
#include "TestEngine.h"

class WebApp {
public:
    WebApp(StorageManager& storage, TestEngine& engine);

    bool begin(String& error);
    void loop();
    String ipAddress() const;

private:
    StorageManager& storage_;
    TestEngine& engine_;
    EthernetServer server_;
    uint32_t lastMaintainMs_ = 0;

    void resetW5500();
    void handleClient(EthernetClient& client);
    void route(EthernetClient& client, const String& method, const String& target);

    void sendJson(EthernetClient& client, int statusCode, const String& json);
    void sendText(EthernetClient& client, int statusCode, const String& text);
    void sendFile(EthernetClient& client, File& file, const String& contentType, bool attachment = false, const String& fileName = "");
    void sendHeader(EthernetClient& client, int statusCode, const String& contentType, size_t contentLength, const String& extraHeaders = "");

    static String queryValue(const String& target, const String& key);
    static String urlDecode(const String& value);
    static String pathOnly(const String& target);
    static const char* statusText(int statusCode);
};
