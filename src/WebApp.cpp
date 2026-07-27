#include "WebApp.h"

#include <ArduinoJson.h>

#include "AppConfig.h"

WebApp::WebApp(StorageManager& storage, TestEngine& engine)
    : storage_(storage), engine_(engine), server_(AppConfig::HTTP_PORT) {}

bool WebApp::begin(String& error) {
    resetW5500();

    SPI.begin(AppConfig::W5500_SCK_PIN,
              AppConfig::W5500_MISO_PIN,
              AppConfig::W5500_MOSI_PIN,
              AppConfig::W5500_CS_PIN);
    Ethernet.init(AppConfig::W5500_CS_PIN);

    uint8_t mac[6];
    memcpy(mac, AppConfig::MAC_ADDRESS, sizeof(mac));

    if (AppConfig::USE_DHCP) {
        if (Ethernet.begin(mac, 8000, 3000) == 0) {
            Ethernet.begin(mac,
                           AppConfig::STATIC_IP,
                           AppConfig::STATIC_DNS,
                           AppConfig::STATIC_GATEWAY,
                           AppConfig::STATIC_SUBNET);
        }
    } else {
        Ethernet.begin(mac,
                       AppConfig::STATIC_IP,
                       AppConfig::STATIC_DNS,
                       AppConfig::STATIC_GATEWAY,
                       AppConfig::STATIC_SUBNET);
    }

    delay(500);
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
        error = "W5500 не обнаружен";
        return false;
    }

    server_.begin();
    return true;
}

void WebApp::resetW5500() {
    if (AppConfig::W5500_RESET_PIN < 0) return;
    pinMode(AppConfig::W5500_RESET_PIN, OUTPUT);
    digitalWrite(AppConfig::W5500_RESET_PIN, LOW);
    delay(5);
    digitalWrite(AppConfig::W5500_RESET_PIN, HIGH);
    delay(100);
}

void WebApp::loop() {
    EthernetClient client = server_.available();
    if (client) {
        handleClient(client);
    }

    if (millis() - lastMaintainMs_ >= 1000) {
        lastMaintainMs_ = millis();
        Ethernet.maintain();
    }
}

String WebApp::ipAddress() const {
    const IPAddress ip = Ethernet.localIP();
    return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

void WebApp::handleClient(EthernetClient& client) {
    client.setTimeout(1000);
    const String requestLine = client.readStringUntil('\n');
    if (requestLine.length() > 2048) {
        sendText(client, 414, "Request URI too long");
        client.stop();
        return;
    }

    int firstSpace = requestLine.indexOf(' ');
    int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
    if (firstSpace <= 0 || secondSpace <= firstSpace) {
        sendText(client, 400, "Bad request");
        client.stop();
        return;
    }

    const String method = requestLine.substring(0, firstSpace);
    const String target = requestLine.substring(firstSpace + 1, secondSpace);

    while (client.connected()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
    }

    route(client, method, target);
    delay(1);
    client.stop();
}

void WebApp::route(EthernetClient& client, const String& method, const String& target) {
    const String path = pathOnly(target);

    if (method == "GET" && path == "/api/status") {
        sendJson(client, 200, engine_.statusJson());
        return;
    }

    if (method == "GET" && path == "/api/device") {
        JsonDocument doc;
        doc["ip"] = ipAddress();
        doc["link"] = Ethernet.linkStatus() == LinkON;
        doc["hardware"] = Ethernet.hardwareStatus() == EthernetW5500 ? "W5500" : "unknown";
        doc["freeHeap"] = ESP.getFreeHeap();
        doc["freePsram"] = ESP.getFreePsram();
        doc["flashSize"] = ESP.getFlashChipSize();
        String json;
        serializeJson(doc, json);
        sendJson(client, 200, json);
        return;
    }

    if (method == "GET" && path == "/api/references") {
        sendJson(client, 200, storage_.listReferencesJson());
        return;
    }

    if (method == "GET" && path == "/api/results") {
        sendJson(client, 200, storage_.listResultsJson());
        return;
    }

    if (method == "POST" && path == "/api/reference/capture") {
        const String name = queryValue(target, "name");
        String error;
        if (!engine_.startReferenceCapture(name, error)) {
            sendJson(client, 409, "{\"ok\":false,\"error\":\"" + error + "\"}");
        } else {
            sendJson(client, 202, "{\"ok\":true}");
        }
        return;
    }

    if (method == "POST" && path == "/api/test/start") {
        const String reference = queryValue(target, "reference");
        String error;
        if (!engine_.startComparison(reference, error)) {
            sendJson(client, 409, "{\"ok\":false,\"error\":\"" + error + "\"}");
        } else {
            sendJson(client, 202, "{\"ok\":true}");
        }
        return;
    }

    if (method == "GET" && path == "/api/result") {
        const String fileName = queryValue(target, "file");
        String json;
        String error;
        if (!storage_.readResult(fileName, json, error)) {
            sendJson(client, 404, "{\"ok\":false,\"error\":\"" + error + "\"}");
        } else {
            sendJson(client, 200, json);
        }
        return;
    }

    if (method == "GET" && path == "/api/download") {
        const String type = queryValue(target, "type");
        const String fileName = queryValue(target, "file");
        if (!StorageManager::safeFileName(fileName)) {
            sendText(client, 400, "Invalid file name");
            return;
        }
        String relativePath;
        if (type == "reference") relativePath = String(AppConfig::REFERENCE_DIR) + "/" + fileName;
        else if (type == "result") relativePath = String(AppConfig::RESULT_DIR) + "/" + fileName;
        else {
            sendText(client, 400, "Invalid file type");
            return;
        }

        File file;
        String contentType;
        if (!storage_.openDataFile(relativePath, file, contentType)) {
            sendText(client, 404, "File not found");
            return;
        }
        sendFile(client, file, contentType, true, fileName);
        file.close();
        return;
    }

    if (method != "GET") {
        sendText(client, 405, "Method not allowed");
        return;
    }

    File file;
    String contentType;
    if (!storage_.openStaticFile(path, file, contentType)) {
        sendText(client, 404, "Not found");
        return;
    }
    sendFile(client, file, contentType);
    file.close();
}

void WebApp::sendHeader(EthernetClient& client,
                        int statusCode,
                        const String& contentType,
                        size_t contentLength,
                        const String& extraHeaders) {
    client.print("HTTP/1.1 ");
    client.print(statusCode);
    client.print(' ');
    client.println(statusText(statusCode));
    client.print("Content-Type: ");
    client.println(contentType);
    client.print("Content-Length: ");
    client.println(contentLength);
    client.println("Connection: close");
    client.println("Cache-Control: no-store");
    client.println("Access-Control-Allow-Origin: *");
    if (extraHeaders.length()) client.print(extraHeaders);
    client.println();
}

void WebApp::sendJson(EthernetClient& client, int statusCode, const String& json) {
    sendHeader(client, statusCode, "application/json; charset=utf-8", json.length());
    client.print(json);
}

void WebApp::sendText(EthernetClient& client, int statusCode, const String& text) {
    sendHeader(client, statusCode, "text/plain; charset=utf-8", text.length());
    client.print(text);
}

void WebApp::sendFile(EthernetClient& client,
                      File& file,
                      const String& contentType,
                      bool attachment,
                      const String& fileName) {
    String extra;
    if (attachment) {
        extra = "Content-Disposition: attachment; filename=\"" + fileName + "\"\r\n";
    }
    sendHeader(client, 200, contentType, file.size(), extra);

    uint8_t buffer[1024];
    while (file.available() && client.connected()) {
        const size_t count = file.read(buffer, sizeof(buffer));
        if (count == 0) break;
        client.write(buffer, count);
    }
}

String WebApp::pathOnly(const String& target) {
    const int query = target.indexOf('?');
    return query < 0 ? target : target.substring(0, query);
}

String WebApp::queryValue(const String& target, const String& key) {
    const int queryStart = target.indexOf('?');
    if (queryStart < 0) return "";

    int cursor = queryStart + 1;
    while (cursor < static_cast<int>(target.length())) {
        int amp = target.indexOf('&', cursor);
        if (amp < 0) amp = target.length();
        const String pair = target.substring(cursor, amp);
        const int equals = pair.indexOf('=');
        const String pairKey = equals < 0 ? pair : pair.substring(0, equals);
        if (pairKey == key) {
            return urlDecode(equals < 0 ? "" : pair.substring(equals + 1));
        }
        cursor = amp + 1;
    }
    return "";
}

String WebApp::urlDecode(const String& value) {
    String result;
    result.reserve(value.length());
    for (size_t i = 0; i < value.length(); ++i) {
        if (value[i] == '+') {
            result += ' ';
        } else if (value[i] == '%' && i + 2 < value.length()) {
            char hex[3] = {value[i + 1], value[i + 2], 0};
            result += static_cast<char>(strtol(hex, nullptr, 16));
            i += 2;
        } else {
            result += value[i];
        }
    }
    return result;
}

const char* WebApp::statusText(int statusCode) {
    switch (statusCode) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 414: return "URI Too Long";
        default: return "Error";
    }
}
