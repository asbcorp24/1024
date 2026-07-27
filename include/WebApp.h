#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ETH.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "StorageManager.h"
#include "TestEngine.h"

class WebApp {
public:
    WebApp(StorageManager& storage, TestEngine& engine);

    bool begin(String& error);
    String ipAddress() const;
    bool linkUp() const;

private:
    enum class UploadKind : uint8_t {
        Reference,
        Calculation
    };

    struct UploadContext {
        UploadKind kind = UploadKind::Reference;
        String originalName;
        String tempPath;
        String finalPath;
        String savedFile;
        String error;
        size_t received = 0;
        bool finished = false;
        bool success = false;
    };

    StorageManager& storage_;
    TestEngine& engine_;
    AsyncWebServer server_;
    AsyncEventSource events_;
    TaskHandle_t publisherTaskHandle_ = nullptr;
    volatile bool ethStarted_ = false;
    volatile bool ethLinkUp_ = false;
    volatile bool ethHasIp_ = false;

    static WebApp* instance_;
    static void networkEventThunk(arduino_event_id_t event, arduino_event_info_t info);
    static void publisherTaskThunk(void* parameter);

    void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info);
    void publisherTask();
    void configureRoutes();
    void registerUploadRoute(const char* uri, UploadKind kind);
    void handleUploadChunk(AsyncWebServerRequest* request,
                           UploadKind kind,
                           const String& filename,
                           size_t index,
                           uint8_t* data,
                           size_t len,
                           bool final);
    void finishUploadRequest(AsyncWebServerRequest* request);

    void sendDevice(AsyncWebServerRequest* request);
    void sendDownload(AsyncWebServerRequest* request);
    void sendJson(AsyncWebServerRequest* request, int statusCode, const String& json);
    void sendError(AsyncWebServerRequest* request, int statusCode, const String& error);
    void publishStatus();

    static String parameter(AsyncWebServerRequest* request, const char* name);
    static String uploadKindName(UploadKind kind);
};
