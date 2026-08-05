#include "WebApp.h"

#include <ArduinoJson.h>
#include <FFat.h>
#include <LittleFS.h>
#include <Network.h>
#include <cstdlib>
#include <esp32-hal-log.h>
#include <time.h>
#include <esp_heap_caps.h>

#include "AppConfig.h"
#include "BrowserTime.h"

namespace {

constexpr uint8_t MATRIX_CELL_EMPTY = 0;
constexpr uint8_t MATRIX_CELL_CONNECTED = 1;
constexpr uint8_t MATRIX_CELL_MISSING = 2;
constexpr uint8_t MATRIX_CELL_EXTRA = 3;
constexpr uint8_t MATRIX_CELL_ASYMMETRIC = 4;
constexpr uint8_t MATRIX_CELL_MISMATCH = 5;
constexpr size_t MATRIX_VIEW_MAX_CONNECTIONS = 256;

bool readServiceUnlockPin() {
    if (AppConfig::SERVICE_UNLOCK_PIN < 0) return true;
    const bool levelHigh = digitalRead(AppConfig::SERVICE_UNLOCK_PIN) == HIGH;
    return AppConfig::SERVICE_UNLOCK_ACTIVE_LOW ? !levelHigh : levelHigh;
}

bool readServiceUnlockRawHigh() {
    if (AppConfig::SERVICE_UNLOCK_PIN < 0) return true;
    return digitalRead(AppConfig::SERVICE_UNLOCK_PIN) == HIGH;
}

void writeServiceUnlockLed(bool enabled) {
    if (AppConfig::SERVICE_UNLOCK_LED_PIN < 0) return;
#ifdef RGB_BUILTIN
    if (AppConfig::SERVICE_UNLOCK_LED_PIN == RGB_BUILTIN) {
        rgbLedWrite(RGB_BUILTIN, enabled ? 0 : 0, enabled ? 48 : 0, enabled ? 0 : 0);
        return;
    }
#endif
    const uint8_t level =
        (enabled == AppConfig::SERVICE_UNLOCK_LED_ACTIVE_HIGH) ? HIGH : LOW;
    digitalWrite(AppConfig::SERVICE_UNLOCK_LED_PIN, level);
}

String queryParameter(AsyncWebServerRequest* request, const char* name) {
    if (!request->hasParam(name)) return "";
    return request->getParam(name)->value();
}

bool copyRequired(AsyncWebServerRequest* request,
                  const char* parameterName,
                  const char* fieldName,
                  char* destination,
                  size_t destinationSize,
                  String& error) {
    String value = queryParameter(request, parameterName);
    value.trim();
    if (value.isEmpty()) {
        error = String("Ne zapolneno pole: ") + fieldName;
        return false;
    }
    if (value.length() >= destinationSize) {
        error = String("Slishkom dlinnoe pole: ") + fieldName;
        return false;
    }
    strlcpy(destination, value.c_str(), destinationSize);
    return true;
}

bool parseMappingCrc(const String& input, uint32_t& value, String& error) {
    String text = input;
    text.trim();
    if (text.isEmpty()) {
        value = 0;
        return true;
    }
    if (text.startsWith("0x") || text.startsWith("0X")) text.remove(0, 2);
    if (text.isEmpty() || text.length() > 8) {
        error = "CRC32 dolzhen soderzhat ne bolee 8 hex-simvolov";
        return false;
    }
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        const bool hex = (c >= '0' && c <= '9') ||
                         (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) {
            error = "CRC32 soderzhit nedopustimye simvoly";
            return false;
        }
    }
    value = static_cast<uint32_t>(strtoul(text.c_str(), nullptr, 16));
    return true;
}

bool validApprovalStatus(const String& status) {
    return status == "draft" || status == "pending" || status == "approved" ||
           status == "rejected" || status == "archived";
}

bool parseBrowserTime(AsyncWebServerRequest* request,
                      BrowserTimeContext& browserTime,
                      String& error) {
    return BrowserTime::parseFromBrowser(queryParameter(request, "epochMs"),
                                         queryParameter(request, "offsetMinutes"),
                                         queryParameter(request, "timeZone"),
                                         browserTime,
                                         error);
}

bool parsePinNumber(const String& input, uint16_t& value) {
    String text = input;
    text.trim();
    if (text.isEmpty()) return false;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
    }
    const unsigned long parsed = strtoul(text.c_str(), nullptr, 10);
    if (parsed >= AppConfig::PIN_COUNT) return false;
    value = static_cast<uint16_t>(parsed);
    return true;
}

bool resolveRequestTime(AsyncWebServerRequest* request,
                        const RtcClock& rtc,
                        BrowserTimeContext& value,
                        String& error) {
    if (rtc.currentTime(value, error)) return true;
    error = "";
    return parseBrowserTime(request, value, error);
}

bool parseReferenceMetadata(AsyncWebServerRequest* request,
                            const RtcClock& rtc,
                            ReferenceCaptureMetadata& metadata,
                            String& error) {
    if (!resolveRequestTime(request, rtc, metadata.time, error)) return false;
    if (!copyRequired(request, "name", "Nazvanie etalona", metadata.name, sizeof(metadata.name), error)) return false;
    if (!copyRequired(request, "cableType", "Tip kabelnoy sborki", metadata.cableType, sizeof(metadata.cableType), error)) return false;
    if (!copyRequired(request, "revision", "Reviziya", metadata.revision, sizeof(metadata.revision), error)) return false;
    if (!copyRequired(request, "deviceId", "Pribor", metadata.deviceId, sizeof(metadata.deviceId), error)) return false;
    if (!copyRequired(request, "operator", "Operator", metadata.operatorName, sizeof(metadata.operatorName), error)) return false;

    String comment = queryParameter(request, "comment");
    comment.trim();
    if (comment.length() >= sizeof(metadata.comment)) {
        error = "Kommentariy prevyshaet 255 simvolov";
        return false;
    }
    strlcpy(metadata.comment, comment.c_str(), sizeof(metadata.comment));

    String approvalStatus = queryParameter(request, "approvalStatus");
    approvalStatus.trim();
    if (!validApprovalStatus(approvalStatus)) {
        error = "Nedopustimyy status utverzhdeniya etalona";
        return false;
    }
    strlcpy(metadata.approvalStatus, approvalStatus.c_str(), sizeof(metadata.approvalStatus));

    return parseMappingCrc(queryParameter(request, "mappingCrc32"), metadata.mappingCrc32, error);
}

bool parseComparisonMetadata(AsyncWebServerRequest* request,
                             const RtcClock& rtc,
                             ComparisonRunMetadata& metadata,
                             String& error) {
    if (!resolveRequestTime(request, rtc, metadata.time, error)) return false;
    if (!copyRequired(request, "cableType", "Tip kabelnoy sborki", metadata.cableType, sizeof(metadata.cableType), error)) return false;
    if (!copyRequired(request, "deviceId", "Pribor", metadata.deviceId, sizeof(metadata.deviceId), error)) return false;
    if (!copyRequired(request, "operator", "Operator", metadata.operatorName, sizeof(metadata.operatorName), error)) return false;
    return true;
}

String jsonEscape(const String& value) {
    String result;
    result.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<uint8_t>(c) >= 0x20) result += c;
                break;
        }
    }
    return result;
}

bool bitAt(const uint8_t* matrix, uint16_t row, uint16_t column) {
    const size_t offset = static_cast<size_t>(row) * AppConfig::ROW_BYTES + (column >> 3);
    return (matrix[offset] & (1U << (column & 0x07))) != 0;
}

void summarizeModulePair(const uint8_t* matrix,
                         uint8_t moduleA,
                         uint8_t moduleB,
                         uint16_t& connectedCount,
                         bool& asymmetric) {
    connectedCount = 0;
    asymmetric = false;
    if (matrix == nullptr) return;

    const uint16_t baseA = static_cast<uint16_t>(moduleA) * AppConfig::PINS_PER_MODULE;
    const uint16_t baseB = static_cast<uint16_t>(moduleB) * AppConfig::PINS_PER_MODULE;
    for (uint16_t offsetA = 0; offsetA < AppConfig::PINS_PER_MODULE; ++offsetA) {
        const uint16_t pinA = baseA + offsetA;
        const uint16_t startOffsetB = moduleA == moduleB ? static_cast<uint16_t>(offsetA + 1) : 0;
        for (uint16_t offsetB = startOffsetB; offsetB < AppConfig::PINS_PER_MODULE; ++offsetB) {
            const uint16_t pinB = baseB + offsetB;
            const bool ab = bitAt(matrix, pinA, pinB);
            const bool ba = bitAt(matrix, pinB, pinA);
            if (ab != ba) asymmetric = true;
            if (ab && ba) ++connectedCount;
        }
    }
}

void appendConnectedPairsJson(String& json,
                              const uint8_t* matrix,
                              size_t& emittedCount) {
    emittedCount = 0;
    json += '[';

    if (matrix != nullptr) {
        bool first = true;
        for (uint16_t a = 0; a < AppConfig::PIN_COUNT; ++a) {
            for (uint16_t b = a + 1; b < AppConfig::PIN_COUNT; ++b) {
                const bool connected = bitAt(matrix, a, b) && bitAt(matrix, b, a);
                if (!connected) continue;

                if (!first) json += ',';
                first = false;
                json += "{\"a\":";
                json += String(a);
                json += ",\"b\":";
                json += String(b);
                json += ",\"settleUs\":";
                json += String(AppConfig::SOURCE_SETTLE_US);
                json += '}';
                ++emittedCount;
            }
        }
    }

    json += ']';
}

String buildReferenceMatrixViewJson(const String& fileName,
                                    const ReferenceFileHeader& header,
                                    const uint8_t* reference) {
    String json;
    json.reserve(22000);
    json += "{\"ok\":true,\"kind\":\"reference\"";
    json += ",\"file\":\"" + jsonEscape(fileName) + "\"";
    json += ",\"title\":\"" + jsonEscape(String(header.name)) + "\"";
    json += ",\"subtitle\":\"" + jsonEscape(String(header.cableType) + " rev. " + String(header.revision)) + "\"";
    json += ",\"size\":64";
    json += ",\"legend\":[";
    json += "{\"code\":0,\"label\":\"Нет связи\",\"color\":\"#e2e8f0\"},";
    json += "{\"code\":1,\"label\":\"Есть связь\",\"color\":\"#1f7a3f\"}";
    json += "]";
    json += ",\"cells\":[";

    bool first = true;
    size_t activeCells = 0;
    for (uint8_t row = 0; row < AppConfig::MODULE_COUNT; ++row) {
        for (uint8_t column = 0; column < AppConfig::MODULE_COUNT; ++column) {
            uint16_t connectedCount = 0;
            bool asymmetric = false;
            summarizeModulePair(reference, row, column, connectedCount, asymmetric);
            if (!first) json += ',';
            first = false;
            const uint8_t code = connectedCount > 0 ? MATRIX_CELL_CONNECTED : MATRIX_CELL_EMPTY;
            if (code != MATRIX_CELL_EMPTY) ++activeCells;
            json += String(code);
        }
    }
    json += "]";

    size_t connectionCount = 0;
    json += ",\"connectionCount\":";
    {
        String connectionsJson;
        appendConnectedPairsJson(connectionsJson,
                                 reference,
                                 connectionCount);
        json += String(connectionCount);
        json += ",\"directedConnectionCount\":";
        json += String(connectionCount * 2U);
        json += ",\"activeCellCount\":";
        json += String(activeCells);
        json += ",\"measurementSettleUs\":";
        json += String(AppConfig::SOURCE_SETTLE_US);
        json += ",\"scopeLabel\":\"Укрупнённая карта по 64 модулям; справа показаны точные пары контактов\"";
        json += ",\"connections\":";
        json += connectionsJson;
        json += ",\"connectionsTruncated\":false";
    }

    json += "}";
    return json;
}

String buildResultMatrixViewJson(const String& fileName,
                                 const String& referenceFile,
                                 const String& title,
                                 const uint8_t* reference,
                                 const uint8_t* measured) {
    String json;
    json.reserve(26000);
    json += "{\"ok\":true,\"kind\":\"result\"";
    json += ",\"file\":\"" + jsonEscape(fileName) + "\"";
    json += ",\"reference\":\"" + jsonEscape(referenceFile) + "\"";
    json += ",\"title\":\"" + jsonEscape(title) + "\"";
    json += ",\"size\":64";
    json += ",\"legend\":[";
    json += "{\"code\":0,\"label\":\"Нет связи\",\"color\":\"#e2e8f0\"},";
    json += "{\"code\":1,\"label\":\"Совпадает\",\"color\":\"#1f7a3f\"},";
    json += "{\"code\":2,\"label\":\"Не хватает связи\",\"color\":\"#c0392b\"},";
    json += "{\"code\":3,\"label\":\"Лишняя связь\",\"color\":\"#1f6fd6\"},";
    json += "{\"code\":4,\"label\":\"Асимметрия\",\"color\":\"#d9aa00\"},";
    json += "{\"code\":5,\"label\":\"Смешанное отличие\",\"color\":\"#0f4c81\"}";
    json += "]";
    json += ",\"cells\":[";

    bool first = true;
    size_t activeCells = 0;
    for (uint8_t row = 0; row < AppConfig::MODULE_COUNT; ++row) {
        for (uint8_t column = 0; column < AppConfig::MODULE_COUNT; ++column) {
            uint16_t expectedCount = 0;
            uint16_t measuredCount = 0;
            bool expectedAsymmetric = false;
            bool measuredAsymmetric = false;
            summarizeModulePair(reference, row, column, expectedCount, expectedAsymmetric);
            summarizeModulePair(measured, row, column, measuredCount, measuredAsymmetric);

            uint8_t code = MATRIX_CELL_EMPTY;
            if (measuredAsymmetric) {
                code = MATRIX_CELL_ASYMMETRIC;
            } else if (expectedCount == 0 && measuredCount == 0) {
                code = MATRIX_CELL_EMPTY;
            } else if (expectedCount == measuredCount) {
                code = expectedCount > 0 ? MATRIX_CELL_CONNECTED : MATRIX_CELL_EMPTY;
            } else if (expectedCount > 0 && measuredCount == 0) {
                code = MATRIX_CELL_MISSING;
            } else if (expectedCount == 0 && measuredCount > 0) {
                code = MATRIX_CELL_EXTRA;
            } else {
                code = MATRIX_CELL_MISMATCH;
            }

            if (!first) json += ',';
            first = false;
            if (code != MATRIX_CELL_EMPTY) ++activeCells;
            json += String(code);
        }
    }
    json += "]";

    size_t connectionCount = 0;
    json += ",\"connectionCount\":";
    {
        String connectionsJson;
        appendConnectedPairsJson(connectionsJson,
                                 measured,
                                 connectionCount);
        json += String(connectionCount);
        json += ",\"directedConnectionCount\":";
        json += String(connectionCount * 2U);
        json += ",\"activeCellCount\":";
        json += String(activeCells);
        json += ",\"measurementSettleUs\":";
        json += String(AppConfig::SOURCE_SETTLE_US);
        json += ",\"scopeLabel\":\"Укрупнённая карта по 64 модулям; справа показаны фактически найденные пары контактов\"";
        json += ",\"connections\":";
        json += connectionsJson;
        json += ",\"connectionsTruncated\":false";
    }

    json += "}";
    return json;
}

} // namespace

WebApp* WebApp::instance_ = nullptr;

WebApp::WebApp(StorageManager& storage, TestEngine& engine, RtcClock& rtc)
    : storage_(storage),
      engine_(engine),
      rtc_(rtc),
      server_(AppConfig::HTTP_PORT),
      events_("/api/events") {}

bool WebApp::begin(String& error) {
    if (AppConfig::SERVICE_UNLOCK_PIN >= 0) {
        pinMode(AppConfig::SERVICE_UNLOCK_PIN, INPUT_PULLUP);
    }
    if (AppConfig::SERVICE_UNLOCK_LED_PIN >= 0) {
        pinMode(AppConfig::SERVICE_UNLOCK_LED_PIN, OUTPUT);
    }
    updateServiceUnlockLed();

    instance_ = this;
    Network.onEvent(networkEventThunk);

    SPI.begin(AppConfig::W5500_SCK_PIN,
              AppConfig::W5500_MISO_PIN,
              AppConfig::W5500_MOSI_PIN,
              AppConfig::W5500_CS_PIN);

    if (!ETH.begin(ETH_PHY_W5500,
                   AppConfig::W5500_PHY_ADDRESS,
                   AppConfig::W5500_CS_PIN,
                   AppConfig::W5500_INT_PIN,
                   AppConfig::W5500_RESET_PIN,
                   SPI)) {
        error = "Ne udalos zapustit W5500 cherez sistemnyy drayver ETH";
        return false;
    }

    ETH.setHostname(AppConfig::HOSTNAME);
    if (!AppConfig::USE_DHCP) {
        if (!ETH.config(AppConfig::STATIC_IP,
                        AppConfig::STATIC_GATEWAY,
                        AppConfig::STATIC_SUBNET,
                        AppConfig::STATIC_DNS)) {
            error = "Ne udalos primenit staticheskie parametry Ethernet";
            return false;
        }
    }

    configureRoutes();
    server_.addHandler(&events_);
    server_.serveStatic("/", LittleFS, "/")
        .setDefaultFile("index.html")
        .setCacheControl("no-store");

    const bool indexExists = LittleFS.exists("/index.html");
    log_i("LittleFS check: /index.html exists = %s", indexExists ? "true" : "false");
    if (indexExists) {
        File indexFile = LittleFS.open("/index.html", FILE_READ);
        if (indexFile && !indexFile.isDirectory()) {
            log_i("LittleFS check: /index.html size = %u", static_cast<unsigned>(indexFile.size()));
        } else {
            log_w("LittleFS check: /index.html exists but open failed");
        }
        if (indexFile) indexFile.close();
    } else {
        log_w("LittleFS check: /index.html not found");
    }

    server_.onNotFound([this](AsyncWebServerRequest* request) {
        logRequest(request, 404);
        request->send(404, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"Маршрут не найден\"}");
    });

    server_.begin();

    if (xTaskCreatePinnedToCore(publisherTaskThunk,
                                "web-events",
                                6144,
                                this,
                                1,
                                &publisherTaskHandle_,
                                1) != pdPASS) {
        error = "HTTP-server zapushchen, no ne udalos sozdat zadachu SSE";
        return false;
    }

    return true;
}

void WebApp::networkEventThunk(arduino_event_id_t event, arduino_event_info_t info) {
    if (instance_ != nullptr) instance_->onNetworkEvent(event, info);
}

void WebApp::onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
    (void)info;
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            ethStarted_ = true;
            Serial.println("ETH: W5500 started");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            ethLinkUp_ = true;
            Serial.println("ETH: link connected");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            ethHasIp_ = true;
            ethLinkUp_ = true;
            Serial.println("ETH: IP " + ipAddress());
            break;
        case ARDUINO_EVENT_ETH_LOST_IP:
            ethHasIp_ = false;
            Serial.println("ETH: IP lost");
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            ethLinkUp_ = false;
            ethHasIp_ = false;
            Serial.println("ETH: link disconnected");
            break;
        case ARDUINO_EVENT_ETH_STOP:
            ethStarted_ = false;
            ethLinkUp_ = false;
            ethHasIp_ = false;
            Serial.println("ETH: stopped");
            break;
        default:
            break;
    }
}

String WebApp::ipAddress() const {
    return ETH.localIP().toString();
}

bool WebApp::linkUp() const {
    return ethLinkUp_ && ETH.linkUp();
}

String WebApp::currentClockText() const {
    const time_t nowSeconds = time(nullptr);
    if (nowSeconds >= 946684800) {
        tm nowTm {};
        if (gmtime_r(&nowSeconds, &nowTm) != nullptr) {
            char buffer[16];
            snprintf(buffer,
                     sizeof(buffer),
                     "TIME: %02d:%02d:%02d",
                     nowTm.tm_hour,
                     nowTm.tm_min,
                     nowTm.tm_sec);
            return String(buffer);
        }
    }

    if (!rtc_.synced()) return "TIME: --:--:--";

    const BrowserTimeContext& synced = rtc_.lastSync();
    const String fallbackIso = BrowserTime::formatLocal(synced.startedAtEpochMs, synced.utcOffsetMinutes);
    const int timeSeparator = fallbackIso.indexOf('T');
    if (timeSeparator < 0 || fallbackIso.length() < timeSeparator + 9) return "TIME: --:--:--";
    return "TIME: " + fallbackIso.substring(timeSeparator + 1, timeSeparator + 9);
}

void WebApp::publisherTaskThunk(void* parameter) {
    static_cast<WebApp*>(parameter)->publisherTask();
}

void WebApp::publisherTask() {
    for (;;) {
        updateServiceUnlockLed();
        publishStatus();
        vTaskDelay(pdMS_TO_TICKS(350));
    }
}

void WebApp::publishStatus() {
    const bool enabled = serviceUnlockEnabled();
    if (!serviceUnlockStateKnown_ || enabled != serviceUnlockLastState_) {
        serviceUnlockLastState_ = enabled;
        serviceUnlockStateKnown_ = true;
        log_i("SERVICE_UNLOCK: pin=%d raw=%s activeLow=%s enabled=%s",
              static_cast<int>(AppConfig::SERVICE_UNLOCK_PIN),
              serviceUnlockRawHigh() ? "HIGH" : "LOW",
              AppConfig::SERVICE_UNLOCK_ACTIVE_LOW ? "true" : "false",
              enabled ? "true" : "false");
    }

    if (events_.count() == 0) return;
    const String json = engine_.statusJson();
    events_.send(json.c_str(), "status", millis(), 1000);
}

void WebApp::configureRoutes() {
    events_.onConnect([this](AsyncEventSourceClient* client) {
        const String json = engine_.statusJson();
        client->send(json.c_str(), "status", millis(), 1000);
    });

    server_.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendJson(request, 200, engine_.statusJson());
    });

    server_.on("/api/device", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendDevice(request);
    });

    server_.on("/api/time", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendClockStatus(request);
    });

    server_.on("/api/diagnostic/pin", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendSinglePinScan(request);
    });

    server_.on("/api/time/sync", HTTP_POST, [this](AsyncWebServerRequest* request) {
        String error;
        BrowserTimeContext browserTime;
        if (!parseBrowserTime(request, browserTime, error)) {
            sendError(request, 400, error);
            return;
        }
        if (!rtc_.syncFromBrowser(browserTime, error)) {
            sendError(request, 500, error);
            return;
        }

        JsonDocument doc;
        doc["ok"] = true;
        doc["source"] = "browser";
        doc["epochMs"] = browserTime.startedAtEpochMs;
        doc["utcOffsetMinutes"] = browserTime.utcOffsetMinutes;
        doc["timeZone"] = browserTime.timeZone;
        doc["utc"] = BrowserTime::formatUtc(browserTime.startedAtEpochMs);
        doc["local"] = BrowserTime::formatLocal(browserTime.startedAtEpochMs,
                                                browserTime.utcOffsetMinutes);
        String json;
        serializeJson(doc, json);
        sendJson(request, 200, json);
    });

    server_.on("/api/references", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendJson(request, 200, storage_.listReferencesJson());
    });

    server_.on("/api/reference", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        deleteReference(request);
    });

    server_.on("/api/reference/view", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendReferenceView(request);
    });

    server_.on("/api/calculations", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendJson(request,
                 200,
                 storage_.listCalculationsJson(parameter(request, "dateFrom"),
                                               parameter(request, "dateTo"),
                                               parameter(request, "deviceId"),
                                               parameter(request, "operator")));
    });

    server_.on("/api/calculation", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        deleteCalculation(request);
    });

    server_.on("/api/results", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendJson(request, 200, storage_.listResultsJson());
    });

    server_.on("/api/reference/capture", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (AppConfig::UI_ONLY_MODE) {
            sendError(request, 409, "UI-only mode: reference capture is disabled until TCA9548A is connected");
            return;
        }
        if (!serviceUnlockEnabled()) {
            sendError(request, 403, serviceLockError());
            return;
        }
        String error;
        ReferenceCaptureMetadata metadata;
        if (!parseReferenceMetadata(request, rtc_, metadata, error)) {
            sendError(request, 400, error);
            return;
        }
        if (!engine_.startReferenceCapture(metadata, error)) {
            sendError(request, 409, error);
            return;
        }
        sendJson(request, 202, "{\"ok\":true,\"message\":\"Эталон v3 запущен\",\"timeSource\":\"browser\"}");
    });

    server_.on("/api/test/start", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (AppConfig::UI_ONLY_MODE) {
            sendError(request, 409, "UI-only mode: testing is disabled until TCA9548A is connected");
            return;
        }
        String error;
        ComparisonRunMetadata metadata;
        if (!parseComparisonMetadata(request, rtc_, metadata, error)) {
            sendError(request, 400, error);
            return;
        }
        if (!engine_.startComparison(parameter(request, "reference"), metadata, error)) {
            sendError(request, 409, error);
            return;
        }
        sendJson(request, 202, "{\"ok\":true,\"message\":\"Проверка запущена\",\"timeSource\":\"browser\"}");
    });

    server_.on("/api/result", HTTP_GET, [this](AsyncWebServerRequest* request) {
        String json;
        String error;
        if (!storage_.readResult(parameter(request, "file"), json, error)) {
            sendError(request, 404, error);
            return;
        }
        sendJson(request, 200, json);
    });

    server_.on("/api/result/view", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendResultView(request);
    });

    server_.on("/api/download", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendDownload(request);
    });

    registerUploadRoute("/api/upload/reference", UploadKind::Reference);
    registerUploadRoute("/api/upload/calculation", UploadKind::Calculation);

    server_.on("/api/events", HTTP_OPTIONS, [this](AsyncWebServerRequest* request) {
        sendJson(request, 204, "");
    });
    server_.on("/api/upload/reference", HTTP_OPTIONS, [this](AsyncWebServerRequest* request) {
        sendJson(request, 204, "");
    });
    server_.on("/api/upload/calculation", HTTP_OPTIONS, [this](AsyncWebServerRequest* request) {
        sendJson(request, 204, "");
    });
}

void WebApp::registerUploadRoute(const char* uri, UploadKind kind) {
    server_.on(uri,
               HTTP_POST,
               [this](AsyncWebServerRequest* request) {
                   finishUploadRequest(request);
               },
               [this, kind](AsyncWebServerRequest* request,
                            const String& filename,
                            size_t index,
                            uint8_t* data,
                            size_t len,
                            bool final) {
                   handleUploadChunk(request, kind, filename, index, data, len, final);
               });
}

void WebApp::handleUploadChunk(AsyncWebServerRequest* request,
                               UploadKind kind,
                               const String& filename,
                               size_t index,
                               uint8_t* data,
                               size_t len,
                               bool final) {
    auto* context = static_cast<UploadContext*>(request->_tempObject);

    if (index == 0) {
        context = new (std::nothrow) UploadContext();
        request->_tempObject = context;
        if (context == nullptr) return;

        context->kind = kind;
        context->originalName = filename;
        if (!serviceUnlockEnabled()) {
            context->error = serviceLockError();
            context->finished = final;
            return;
        }
        if (!storage_.prepareUpload(uploadKindName(kind),
                                    filename,
                                    context->tempPath,
                                    context->finalPath,
                                    context->error)) {
            context->finished = final;
            return;
        }

        request->_tempFile = FFat.open(context->tempPath, FILE_WRITE);
        if (!request->_tempFile) {
            context->error = "Ne udalos otkryt vremennyy fayl zagruzki";
            context->finished = final;
            return;
        }
    }

    if (context == nullptr) return;

    if (context->error.isEmpty() && len > 0) {
        const size_t written = request->_tempFile.write(data, len);
        context->received += written;
        if (written != len) context->error = "Zagruzhennyy fayl zapisan ne polnostyu";
    }

    if (!final) return;

    if (request->_tempFile) {
        request->_tempFile.flush();
        request->_tempFile.close();
    }

    context->finished = true;
    if (!context->error.isEmpty()) {
        storage_.discardUpload(context->tempPath);
        return;
    }

    context->success = storage_.finalizeUpload(uploadKindName(kind),
                                               context->tempPath,
                                               context->finalPath,
                                               context->savedFile,
                                               context->error);
}

void WebApp::finishUploadRequest(AsyncWebServerRequest* request) {
    auto* context = static_cast<UploadContext*>(request->_tempObject);
    if (context == nullptr) {
        sendError(request, 400, "Fayl ne peredan");
        return;
    }

    if (!serviceUnlockEnabled()) {
        if (request->_tempFile) request->_tempFile.close();
        storage_.discardUpload(context->tempPath);
        sendError(request, 403, serviceLockError());
        delete context;
        request->_tempObject = nullptr;
        return;
    }

    if (!context->finished) {
        if (request->_tempFile) request->_tempFile.close();
        storage_.discardUpload(context->tempPath);
        sendError(request, 400, "Zagruzka fayla ne zavershena");
    } else if (!context->success) {
        sendError(request, 422, context->error.isEmpty() ? "Fayl ne prinyat" : context->error);
    } else {
        JsonDocument doc;
        doc["ok"] = true;
        doc["file"] = context->savedFile;
        doc["bytes"] = context->received;
        doc["type"] = uploadKindName(context->kind);
        String json;
        serializeJson(doc, json);
        sendJson(request, 201, json);
        events_.send("{\"changed\":true}", "files", millis());
    }

    delete context;
    request->_tempObject = nullptr;
}

void WebApp::sendDevice(AsyncWebServerRequest* request) {
    JsonDocument doc;
    const EngineSnapshot snapshot = engine_.snapshot();
    doc["ip"] = ipAddress();
    doc["hostname"] = AppConfig::HOSTNAME;
    doc["link"] = linkUp();
    doc["hasIp"] = ethHasIp_;
    doc["started"] = ethStarted_;
    doc["hardware"] = "W5500";
    doc["deviceModel"] = AppConfig::DEVICE_MODEL;
    doc["firmwareVersion"] = AppConfig::FIRMWARE_VERSION;
    doc["referenceFormatVersion"] = AppConfig::FILE_FORMAT_VERSION;
    doc["mac"] = ETH.macAddress();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["freePsram"] = ESP.getFreePsram();
    doc["flashSize"] = ESP.getFlashChipSize();
    doc["storageTotalBytes"] = static_cast<uint64_t>(storage_.totalBytes());
    doc["storageUsedBytes"] = static_cast<uint64_t>(storage_.usedBytes());
    doc["storageFreeBytes"] = static_cast<uint64_t>(storage_.totalBytes() - storage_.usedBytes());
    doc["asyncWeb"] = true;
    doc["timeSource"] = "browser";
    doc["uiOnlyMode"] = AppConfig::UI_ONLY_MODE;
    doc["serviceUnlockEnabled"] = serviceUnlockEnabled();
    doc["serviceUnlockPin"] = AppConfig::SERVICE_UNLOCK_PIN;
    doc["serviceUnlockRawHigh"] = serviceUnlockRawHigh();
    doc["serviceUnlockActiveLow"] = AppConfig::SERVICE_UNLOCK_ACTIVE_LOW;
    doc["engineState"] = testStateName(snapshot.state);
    doc["engineMode"] = testModeName(snapshot.mode);
    doc["engineBusy"] = engine_.isBusy();
    doc["engineMessage"] = snapshot.message;
    doc["engineReference"] = snapshot.activeReference;
    doc["engineLastResult"] = snapshot.lastResultFile;
    doc["clockSynced"] = rtc_.synced();
    doc["clockSyncSource"] = rtc_.sourceName();
    doc["rtcPresent"] = rtc_.available();
    if (rtc_.synced()) {
        const BrowserTimeContext& time = rtc_.lastSync();
        doc["clockLastSyncEpochMs"] = time.startedAtEpochMs;
        doc["clockLastSyncUtc"] = BrowserTime::formatUtc(time.startedAtEpochMs);
        doc["clockLastSyncLocal"] = BrowserTime::formatLocal(time.startedAtEpochMs,
                                                             time.utcOffsetMinutes);
        doc["clockTimeZone"] = time.timeZone;
        doc["clockUtcOffsetMinutes"] = time.utcOffsetMinutes;
    }

    String json;
    serializeJson(doc, json);
    sendJson(request, 200, json);
}

void WebApp::sendClockStatus(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["present"] = rtc_.available();
    doc["synced"] = rtc_.synced();
    doc["source"] = rtc_.sourceName();
    if (rtc_.synced()) {
        const BrowserTimeContext& time = rtc_.lastSync();
        doc["epochMs"] = time.startedAtEpochMs;
        doc["utcOffsetMinutes"] = time.utcOffsetMinutes;
        doc["timeZone"] = time.timeZone;
        doc["utc"] = BrowserTime::formatUtc(time.startedAtEpochMs);
        doc["local"] = BrowserTime::formatLocal(time.startedAtEpochMs,
                                                time.utcOffsetMinutes);
    }
    String json;
    serializeJson(doc, json);
    sendJson(request, 200, json);
}

void WebApp::deleteReference(AsyncWebServerRequest* request) {
    if (!serviceUnlockEnabled()) {
        sendError(request, 403, serviceLockError());
        return;
    }
    const String fileName = parameter(request, "file");
    String error;
    if (!storage_.deleteReference(fileName, error)) {
        sendError(request, 404, error);
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    doc["file"] = fileName;
    String json;
    serializeJson(doc, json);
    sendJson(request, 200, json);
    events_.send("{\"changed\":true}", "files", millis());
}

void WebApp::deleteCalculation(AsyncWebServerRequest* request) {
    if (!serviceUnlockEnabled()) {
        sendError(request, 403, serviceLockError());
        return;
    }
    const String fileName = parameter(request, "file");
    String error;
    if (!storage_.deleteCalculation(fileName, error)) {
        sendError(request, 404, error);
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    doc["file"] = fileName;
    String json;
    serializeJson(doc, json);
    sendJson(request, 200, json);
    events_.send("{\"changed\":true}", "files", millis());
}

void WebApp::sendSinglePinScan(AsyncWebServerRequest* request) {
    uint16_t sourcePin = 0;
    if (!parsePinNumber(parameter(request, "source"), sourcePin)) {
        sendError(request, 400, "Укажите source от 0 до 1023");
        return;
    }

    String json;
    String error;
    if (!engine_.scanSingleSource(sourcePin, json, error)) {
        sendError(request, 409, error);
        return;
    }
    sendJson(request, 200, json);
}

void WebApp::sendReferenceView(AsyncWebServerRequest* request) {
    const String fileName = parameter(request, "file");
    uint8_t* reference = static_cast<uint8_t*>(heap_caps_malloc(AppConfig::MATRIX_BYTES,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (reference == nullptr) reference = static_cast<uint8_t*>(malloc(AppConfig::MATRIX_BYTES));
    if (reference == nullptr) {
        sendError(request, 500, "Недостаточно памяти для визуализации эталона");
        return;
    }

    ReferenceFileHeader header {};
    String error;
    if (!storage_.loadReference(fileName, reference, header, error)) {
        free(reference);
        sendError(request, 404, error);
        return;
    }

    const String json = buildReferenceMatrixViewJson(fileName, header, reference);
    free(reference);
    sendJson(request, 200, json);
}

void WebApp::sendResultView(AsyncWebServerRequest* request) {
    const String fileName = parameter(request, "file");
    log_i("WebApp: sendResultView start file=%s", fileName.c_str());
    String reportText;
    String error;
    if (!storage_.readResult(fileName, reportText, error)) {
        log_e("WebApp: sendResultView readResult failed file=%s error=%s",
              fileName.c_str(),
              error.c_str());
        sendError(request, 404, error);
        return;
    }

    JsonDocument report;
    const DeserializationError jsonError = deserializeJson(report, reportText);
    if (jsonError) {
        log_e("WebApp: sendResultView json parse failed file=%s error=%s",
              fileName.c_str(),
              jsonError.c_str());
        sendError(request, 500, String("Не удалось разобрать JSON результата: ") + jsonError.c_str());
        return;
    }

    const String referenceFile = report["reference"] | "";
    const String packedMatrix = report["measurementMatrixPacked"] | "";
    const uint32_t matrixCrc32 = report["measurementMatrixCrc32"] | 0U;
    const String title = String(report["referenceMetadata"]["name"] | fileName.c_str());

    uint8_t* reference = static_cast<uint8_t*>(heap_caps_malloc(AppConfig::MATRIX_BYTES,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t* measured = static_cast<uint8_t*>(heap_caps_malloc(AppConfig::MATRIX_BYTES,
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (reference == nullptr) reference = static_cast<uint8_t*>(malloc(AppConfig::MATRIX_BYTES));
    if (measured == nullptr) measured = static_cast<uint8_t*>(malloc(AppConfig::MATRIX_BYTES));
    if (reference == nullptr || measured == nullptr) {
        log_e("WebApp: sendResultView matrix alloc failed file=%s", fileName.c_str());
        if (reference) free(reference);
        if (measured) free(measured);
        sendError(request, 500, "Недостаточно памяти для визуализации результата");
        return;
    }

    ReferenceFileHeader header {};
    if (!storage_.loadReference(referenceFile, reference, header, error)) {
        log_e("WebApp: sendResultView loadReference failed file=%s ref=%s error=%s",
              fileName.c_str(),
              referenceFile.c_str(),
              error.c_str());
        free(reference);
        free(measured);
        sendError(request, 404, error);
        return;
    }
    if (!storage_.unpackMatrixFromBase64(packedMatrix, measured, matrixCrc32, error)) {
        log_e("WebApp: sendResultView unpackMatrix failed file=%s error=%s packedLen=%u crc32=%08lx",
              fileName.c_str(),
              error.c_str(),
              static_cast<unsigned>(packedMatrix.length()),
              static_cast<unsigned long>(matrixCrc32));
        free(reference);
        free(measured);
        sendError(request, 404, error);
        return;
    }

    const String json = buildResultMatrixViewJson(fileName, referenceFile, title, reference, measured);
    free(reference);
    free(measured);
    log_i("WebApp: sendResultView ok file=%s responseSize=%u",
          fileName.c_str(),
          static_cast<unsigned>(json.length()));
    sendJson(request, 200, json);
}

void WebApp::sendDownload(AsyncWebServerRequest* request) {
    const String type = parameter(request, "type");
    const String fileName = parameter(request, "file");
    String path;
    String contentType;
    String error;

    if (!storage_.resolveDataFile(type, fileName, path, contentType, error)) {
        sendError(request, 404, error);
        return;
    }

    AsyncWebServerResponse* response = request->beginResponse(FFat, path, contentType, true);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("X-Content-Type-Options", "nosniff");
    logRequest(request, 200);
    request->send(response);
}

void WebApp::sendJson(AsyncWebServerRequest* request, int statusCode, const String& json) {
    AsyncWebServerResponse* response = request->beginResponse(statusCode,
                                                              "application/json; charset=utf-8",
                                                              json);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Access-Control-Allow-Origin", "*");
    logRequest(request, statusCode);
    request->send(response);
}

void WebApp::sendError(AsyncWebServerRequest* request, int statusCode, const String& error) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = error;
    String json;
    serializeJson(doc, json);
    sendJson(request, statusCode, json);
}

String WebApp::parameter(AsyncWebServerRequest* request, const char* name) {
    return queryParameter(request, name);
}

String WebApp::uploadKindName(UploadKind kind) {
    return kind == UploadKind::Reference ? "reference" : "calculation";
}

void WebApp::logRequest(AsyncWebServerRequest* request, int statusCode) const {
    if (!AppConfig::HTTP_DEBUG_LOG || request == nullptr) return;
    log_i("HTTP %s %s -> %d",
          methodName(request->method()),
          request->url().c_str(),
          statusCode);
}

bool WebApp::serviceUnlockEnabled() const {
    return readServiceUnlockPin();
}

bool WebApp::serviceUnlockRawHigh() const {
    return readServiceUnlockRawHigh();
}

void WebApp::updateServiceUnlockLed() const {
    writeServiceUnlockLed(serviceUnlockEnabled());
}

String WebApp::serviceLockError() const {
    return "Сервисный тумблер выключен. Разрешите обслуживание на GPIO" +
           String(AppConfig::SERVICE_UNLOCK_PIN) + " и повторите действие";
}

const char* WebApp::methodName(WebRequestMethod method) {
    switch (method) {
        case HTTP_GET: return "GET";
        case HTTP_POST: return "POST";
        case HTTP_DELETE: return "DELETE";
        case HTTP_PUT: return "PUT";
        case HTTP_PATCH: return "PATCH";
        case HTTP_HEAD: return "HEAD";
        case HTTP_OPTIONS: return "OPTIONS";
        default: return "OTHER";
    }
}
