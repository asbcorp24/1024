#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ETH.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "BrowserTime.h"
#include "RtcClock.h"
#include "StorageManager.h"
#include "TestEngine.h"

class WebApp {
public:
    WebApp(StorageManager& storage, TestEngine& engine, RtcClock& rtc);

    bool begin(String& error);
    String ipAddress() const;
    bool linkUp() const;
    String currentClockText() const;

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
    RtcClock& rtc_;
    AsyncWebServer server_;
    AsyncEventSource events_;
    TaskHandle_t publisherTaskHandle_ = nullptr;
    volatile bool ethStarted_ = false;
    volatile bool ethLinkUp_ = false;
    volatile bool ethHasIp_ = false;
    mutable bool serviceUnlockLastState_ = false;
    mutable bool serviceUnlockStateKnown_ = false;

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
    void sendClockStatus(AsyncWebServerRequest* request);
    void deleteReference(AsyncWebServerRequest* request);
    void deleteCalculation(AsyncWebServerRequest* request);
    void sendSinglePinScan(AsyncWebServerRequest* request);
    void sendReferenceView(AsyncWebServerRequest* request);
    void sendResultView(AsyncWebServerRequest* request);
    void sendDownload(AsyncWebServerRequest* request);
    void sendJson(AsyncWebServerRequest* request, int statusCode, const String& json);
    void sendError(AsyncWebServerRequest* request, int statusCode, const String& error);
    void publishStatus();
    void logRequest(AsyncWebServerRequest* request, int statusCode) const;
    bool serviceUnlockEnabled() const;
    bool serviceUnlockRawHigh() const;
    String serviceLockError() const;
    void updateServiceUnlockLed() const;

    static String parameter(AsyncWebServerRequest* request, const char* name);
    static String uploadKindName(UploadKind kind);
    static const char* methodName(WebRequestMethod method);
};
