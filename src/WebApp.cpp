#include "WebApp.h"

#include <ArduinoJson.h>
#include <FFat.h>
#include <LittleFS.h>
#include <Network.h>
#include <cstdlib>

#include "AppConfig.h"
#include "BrowserTime.h"

namespace {

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
        error = String("Не заполнено поле: ") + fieldName;
        return false;
    }
    if (value.length() >= destinationSize) {
        error = String("Слишком длинное поле: ") + fieldName;
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
        error = "CRC32 таблицы должен содержать не более 8 hex-символов";
        return false;
    }
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        const bool hex = (c >= '0' && c <= '9') ||
                         (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) {
            error = "CRC32 таблицы содержит недопустимые символы";
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

bool parseReferenceMetadata(AsyncWebServerRequest* request,
                            ReferenceCaptureMetadata& metadata,
                            String& error) {
    if (!parseBrowserTime(request, metadata.time, error)) return false;
    if (!copyRequired(request, "name", "Название эталона", metadata.name, sizeof(metadata.name), error)) return false;
    if (!copyRequired(request, "cableType", "Тип кабельной сборки", metadata.cableType, sizeof(metadata.cableType), error)) return false;
    if (!copyRequired(request, "revision", "Ревизия", metadata.revision, sizeof(metadata.revision), error)) return false;
    if (!copyRequired(request, "deviceId", "Прибор", metadata.deviceId, sizeof(metadata.deviceId), error)) return false;
    if (!copyRequired(request, "operator", "Оператор", metadata.operatorName, sizeof(metadata.operatorName), error)) return false;

    String comment = queryParameter(request, "comment");
    comment.trim();
    if (comment.length() >= sizeof(metadata.comment)) {
        error = "Комментарий превышает 255 символов";
        return false;
    }
    strlcpy(metadata.comment, comment.c_str(), sizeof(metadata.comment));

    String approvalStatus = queryParameter(request, "approvalStatus");
    approvalStatus.trim();
    if (!validApprovalStatus(approvalStatus)) {
        error = "Недопустимый статус утверждения эталона";
        return false;
    }
    strlcpy(metadata.approvalStatus, approvalStatus.c_str(), sizeof(metadata.approvalStatus));

    return parseMappingCrc(queryParameter(request, "mappingCrc32"), metadata.mappingCrc32, error);
}

} // namespace

WebApp* WebApp::instance_ = nullptr;

WebApp::WebApp(StorageManager& storage, TestEngine& engine)
    : storage_(storage), engine_(engine), server_(AppConfig::HTTP_PORT), events_("/api/events") {}

bool WebApp::begin(String& error) {
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
        error = "Не удалось запустить W5500 через системный драйвер ETH";
        return false;
    }

    ETH.setHostname(AppConfig::HOSTNAME);
    if (!AppConfig::USE_DHCP) {
        if (!ETH.config(AppConfig::STATIC_IP,
                        AppConfig::STATIC_GATEWAY,
                        AppConfig::STATIC_SUBNET,
                        AppConfig::STATIC_DNS)) {
            error = "Не удалось применить статические параметры Ethernet";
            return false;
        }
    }

    configureRoutes();
    server_.addHandler(&events_);
    server_.serveStatic("/", LittleFS, "/")
        .setDefaultFile("index.html")
        .setCacheControl("no-store");

    server_.onNotFound([](AsyncWebServerRequest* request) {
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
        error = "HTTP-сервер запущен, но не удалось создать задачу SSE";
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

void WebApp::publisherTaskThunk(void* parameter) {
    static_cast<WebApp*>(parameter)->publisherTask();
}

void WebApp::publisherTask() {
    for (;;) {
        publishStatus();
        vTaskDelay(pdMS_TO_TICKS(350));
    }
}

void WebApp::publishStatus() {
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

    server_.on("/api/references", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendJson(request, 200, storage_.listReferencesJson());
    });

    server_.on("/api/calculations", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendJson(request, 200, storage_.listCalculationsJson());
    });

    server_.on("/api/results", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendJson(request, 200, storage_.listResultsJson());
    });

    server_.on("/api/reference/capture", HTTP_POST, [this](AsyncWebServerRequest* request) {
        String error;
        ReferenceCaptureMetadata metadata;
        if (!parseReferenceMetadata(request, metadata, error)) {
            sendError(request, 400, error);
            return;
        }
        if (!engine_.startReferenceCapture(metadata, error)) {
            sendError(request, 409, error);
            return;
        }
        sendJson(request, 202, "{\"ok\":true,\"message\":\"Эталон v2 запущен\",\"timeSource\":\"browser\"}");
    });

    server_.on("/api/test/start", HTTP_POST, [this](AsyncWebServerRequest* request) {
        String error;
        BrowserTimeContext browserTime;
        if (!parseBrowserTime(request, browserTime, error)) {
            sendError(request, 400, error);
            return;
        }
        if (!engine_.startComparison(parameter(request, "reference"), browserTime, error)) {
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

    server_.on("/api/download", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendDownload(request);
    });

    registerUploadRoute("/api/upload/reference", UploadKind::Reference);
    registerUploadRoute("/api/upload/calculation", UploadKind::Calculation);

    server_.on("/api/events", HTTP_OPTIONS, [](AsyncWebServerRequest* request) {
        request->send(204);
    });
    server_.on("/api/upload/reference", HTTP_OPTIONS, [](AsyncWebServerRequest* request) {
        request->send(204);
    });
    server_.on("/api/upload/calculation", HTTP_OPTIONS, [](AsyncWebServerRequest* request) {
        request->send(204);
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
            context->error = "Не удалось открыть временный файл загрузки";
            context->finished = final;
            return;
        }
    }

    if (context == nullptr) return;

    if (context->error.isEmpty() && len > 0) {
        const size_t written = request->_tempFile.write(data, len);
        context->received += written;
        if (written != len) context->error = "Загруженный файл записан не полностью";
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
        sendError(request, 400, "Файл не передан");
        return;
    }

    if (!context->finished) {
        if (request->_tempFile) request->_tempFile.close();
        storage_.discardUpload(context->tempPath);
        sendError(request, 400, "Загрузка файла не завершена");
    } else if (!context->success) {
        sendError(request, 422, context->error.isEmpty() ? "Файл не принят" : context->error);
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
    doc["asyncWeb"] = true;
    doc["timeSource"] = "browser";

    String json;
    serializeJson(doc, json);
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
    request->send(response);
}

void WebApp::sendJson(AsyncWebServerRequest* request, int statusCode, const String& json) {
    AsyncWebServerResponse* response = request->beginResponse(statusCode,
                                                              "application/json; charset=utf-8",
                                                              json);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Access-Control-Allow-Origin", "*");
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
