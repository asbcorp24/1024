#include "StorageManager.h"

#include <ArduinoJson.h>
#include <FFat.h>
#include <LittleFS.h>
#include <cstring>

#include "AppConfig.h"

bool StorageManager::begin(String& error) {
    staticMounted_ = LittleFS.begin(true, AppConfig::STATIC_FS_BASE, 10, "littlefs");
    if (!staticMounted_) {
        error = "Не удалось смонтировать LittleFS для веб-статики";
        return false;
    }

    dataMounted_ = FFat.begin(true, AppConfig::DATA_FS_BASE, 10, "ffat");
    if (!dataMounted_) {
        error = "Не удалось смонтировать FFat для эталонов и результатов";
        return false;
    }

    if (!FFat.exists(AppConfig::REFERENCE_DIR) && !FFat.mkdir(AppConfig::REFERENCE_DIR)) {
        error = "Не удалось создать каталог эталонов";
        return false;
    }
    if (!FFat.exists(AppConfig::RESULT_DIR) && !FFat.mkdir(AppConfig::RESULT_DIR)) {
        error = "Не удалось создать каталог расчётов";
        return false;
    }

    cleanupUploadTemps(AppConfig::REFERENCE_DIR);
    cleanupUploadTemps(AppConfig::RESULT_DIR);
    return true;
}

String StorageManager::jsonEscape(const String& value) {
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

String StorageManager::listDirectoryJson(const char* directory,
                                         const char* extensionA,
                                         const char* extensionB) {
    String json = "[";
    bool first = true;
    File dir = FFat.open(directory);
    if (!dir || !dir.isDirectory()) return "[]";

    File file = dir.openNextFile();
    while (file) {
        String fileName = baseFileName(file.name());
        const bool extensionMatches = fileName.endsWith(extensionA) ||
                                      (extensionB != nullptr && fileName.endsWith(extensionB));
        if (!file.isDirectory() && extensionMatches) {
            if (!first) json += ',';
            first = false;
            json += "{\"file\":\"" + jsonEscape(fileName) + "\",";
            json += "\"size\":" + String(static_cast<uint32_t>(file.size()));
            if (fileName.endsWith(".ref")) json += ",\"kind\":\"reference\"";
            else if (fileName.endsWith(".json")) json += ",\"kind\":\"report\"";
            else if (fileName.endsWith(".bin")) json += ",\"kind\":\"matrix\"";
            json += '}';
        }
        file = dir.openNextFile();
    }
    json += ']';
    return json;
}

String StorageManager::listReferencesJson() {
    return listDirectoryJson(AppConfig::REFERENCE_DIR, ".ref");
}

String StorageManager::listResultsJson() {
    return listDirectoryJson(AppConfig::RESULT_DIR, ".json");
}

String StorageManager::listCalculationsJson() {
    return listDirectoryJson(AppConfig::RESULT_DIR, ".json", ".bin");
}

bool StorageManager::saveReference(const String& name,
                                   const uint8_t* matrix,
                                   uint32_t mappingCrc32,
                                   String& savedFile,
                                   String& error) {
    if (!dataMounted_ || matrix == nullptr) {
        error = "Хранилище данных не готово";
        return false;
    }

    const String base = sanitizeBaseName(name);
    const String finalPath = String(AppConfig::REFERENCE_DIR) + "/" + base + ".ref";
    const String tempPath = finalPath + ".tmp";

    ReferenceFileHeader header{};
    header.magic = AppConfig::REFERENCE_MAGIC;
    header.version = AppConfig::FILE_FORMAT_VERSION;
    header.pinCount = AppConfig::PIN_COUNT;
    header.rowBytes = AppConfig::ROW_BYTES;
    header.matrixBytes = AppConfig::MATRIX_BYTES;
    header.matrixCrc32 = crc32(matrix, AppConfig::MATRIX_BYTES);
    header.mappingCrc32 = mappingCrc32;
    header.createdAtMs = millis();
    strlcpy(header.name, name.c_str(), sizeof(header.name));

    FFat.remove(tempPath);
    File file = FFat.open(tempPath, FILE_WRITE);
    if (!file) {
        error = "Не удалось создать временный файл эталона";
        return false;
    }

    const bool ok = file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
                    file.write(matrix, AppConfig::MATRIX_BYTES) == AppConfig::MATRIX_BYTES;
    file.flush();
    file.close();

    if (!ok) {
        FFat.remove(tempPath);
        error = "Ошибка записи матрицы эталона";
        return false;
    }

    FFat.remove(finalPath);
    if (!FFat.rename(tempPath, finalPath)) {
        FFat.remove(tempPath);
        error = "Не удалось завершить сохранение эталона";
        return false;
    }

    savedFile = base + ".ref";
    return true;
}

bool StorageManager::loadReference(const String& fileName,
                                   uint8_t* matrix,
                                   ReferenceFileHeader& header,
                                   String& error) {
    if (!safeFileName(fileName) || !fileName.endsWith(".ref")) {
        error = "Недопустимое имя файла эталона";
        return false;
    }

    const String path = String(AppConfig::REFERENCE_DIR) + "/" + fileName;
    File file = FFat.open(path, FILE_READ);
    if (!file) {
        error = "Файл эталона не найден";
        return false;
    }

    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        file.close();
        error = "Повреждён заголовок эталона";
        return false;
    }

    if (header.magic != AppConfig::REFERENCE_MAGIC ||
        header.version != AppConfig::FILE_FORMAT_VERSION ||
        header.pinCount != AppConfig::PIN_COUNT ||
        header.rowBytes != AppConfig::ROW_BYTES ||
        header.matrixBytes != AppConfig::MATRIX_BYTES) {
        file.close();
        error = "Несовместимый формат файла эталона";
        return false;
    }

    if (file.read(matrix, AppConfig::MATRIX_BYTES) != AppConfig::MATRIX_BYTES) {
        file.close();
        error = "Матрица эталона прочитана не полностью";
        return false;
    }
    file.close();

    if (crc32(matrix, AppConfig::MATRIX_BYTES) != header.matrixCrc32) {
        error = "CRC матрицы эталона не совпадает";
        return false;
    }
    return true;
}

bool StorageManager::saveMeasurement(const String& baseName,
                                     const uint8_t* matrix,
                                     String& savedFile,
                                     String& error) {
    const String base = sanitizeBaseName(baseName);
    const String finalPath = String(AppConfig::RESULT_DIR) + "/" + base + ".bin";
    File file = FFat.open(finalPath, FILE_WRITE);
    if (!file) {
        error = "Не удалось создать бинарный файл измерения";
        return false;
    }
    const bool ok = file.write(matrix, AppConfig::MATRIX_BYTES) == AppConfig::MATRIX_BYTES;
    file.flush();
    file.close();
    if (!ok) {
        error = "Бинарная матрица измерения записана не полностью";
        return false;
    }
    savedFile = base + ".bin";
    return true;
}

bool StorageManager::saveResultReport(const String& baseName,
                                      const String& json,
                                      String& savedFile,
                                      String& error) {
    const String base = sanitizeBaseName(baseName);
    const String finalPath = String(AppConfig::RESULT_DIR) + "/" + base + ".json";
    File file = FFat.open(finalPath, FILE_WRITE);
    if (!file) {
        error = "Не удалось создать JSON-отчёт";
        return false;
    }
    const bool ok = file.print(json) == json.length();
    file.flush();
    file.close();
    if (!ok) {
        error = "JSON-отчёт записан не полностью";
        return false;
    }
    savedFile = base + ".json";
    return true;
}

bool StorageManager::readResult(const String& fileName, String& json, String& error) {
    if (!safeFileName(fileName) || !fileName.endsWith(".json")) {
        error = "Недопустимое имя файла результата";
        return false;
    }
    File file = FFat.open(String(AppConfig::RESULT_DIR) + "/" + fileName, FILE_READ);
    if (!file) {
        error = "Файл результата не найден";
        return false;
    }
    json = file.readString();
    file.close();
    return true;
}

bool StorageManager::prepareUpload(const String& type,
                                   const String& originalFileName,
                                   String& tempPath,
                                   String& finalPath,
                                   String& error) {
    if (!dataMounted_) {
        error = "Хранилище FFat не готово";
        return false;
    }

    const String fileName = baseFileName(originalFileName);
    if (!safeFileName(fileName)) {
        error = "Недопустимое имя загружаемого файла";
        return false;
    }

    String directory;
    if (type == "reference") {
        if (!fileName.endsWith(".ref")) {
            error = "Эталон должен иметь расширение .ref";
            return false;
        }
        directory = AppConfig::REFERENCE_DIR;
    } else if (type == "calculation") {
        if (!fileName.endsWith(".json") && !fileName.endsWith(".bin")) {
            error = "Расчёт должен иметь расширение .json или .bin";
            return false;
        }
        directory = AppConfig::RESULT_DIR;
    } else {
        error = "Неизвестный тип загружаемого файла";
        return false;
    }

    finalPath = directory + "/" + fileName;
    tempPath = directory + "/." + fileName + "." + String(esp_random(), HEX) + ".upload";
    FFat.remove(tempPath);
    return true;
}

bool StorageManager::finalizeUpload(const String& type,
                                    const String& tempPath,
                                    const String& finalPath,
                                    String& savedFile,
                                    String& error) {
    const bool valid = type == "reference"
                           ? validateReferenceFile(tempPath, error)
                           : type == "calculation"
                                 ? validateCalculationFile(tempPath, error)
                                 : false;
    if (!valid) {
        if (error.isEmpty()) error = "Неизвестный тип файла";
        FFat.remove(tempPath);
        return false;
    }

    FFat.remove(finalPath);
    if (!FFat.rename(tempPath, finalPath)) {
        FFat.remove(tempPath);
        error = "Не удалось перенести файл из временного каталога";
        return false;
    }

    savedFile = baseFileName(finalPath);
    return true;
}

void StorageManager::discardUpload(const String& tempPath) {
    if (!tempPath.isEmpty() && tempPath.endsWith(".upload")) FFat.remove(tempPath);
}

bool StorageManager::validateReferenceFile(const String& path, String& error) {
    File file = FFat.open(path, FILE_READ);
    if (!file) {
        error = "Временный файл эталона не найден";
        return false;
    }

    const size_t expectedSize = sizeof(ReferenceFileHeader) + AppConfig::MATRIX_BYTES;
    if (file.size() != expectedSize) {
        file.close();
        error = "Неверный размер файла эталона";
        return false;
    }

    ReferenceFileHeader header{};
    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        file.close();
        error = "Не удалось прочитать заголовок эталона";
        return false;
    }

    if (header.magic != AppConfig::REFERENCE_MAGIC ||
        header.version != AppConfig::FILE_FORMAT_VERSION ||
        header.pinCount != AppConfig::PIN_COUNT ||
        header.rowBytes != AppConfig::ROW_BYTES ||
        header.matrixBytes != AppConfig::MATRIX_BYTES) {
        file.close();
        error = "Загруженный эталон несовместим с устройством";
        return false;
    }

    uint8_t buffer[2048];
    uint32_t crc = 0xFFFFFFFFUL;
    size_t remaining = AppConfig::MATRIX_BYTES;
    while (remaining > 0) {
        const size_t requested = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const size_t count = file.read(buffer, requested);
        if (count == 0) {
            file.close();
            error = "Матрица загруженного эталона обрывается";
            return false;
        }
        crc = crc32Update(crc, buffer, count);
        remaining -= count;
    }
    file.close();

    if ((~crc) != header.matrixCrc32) {
        error = "CRC загруженного эталона не совпадает";
        return false;
    }
    return true;
}

bool StorageManager::validateCalculationFile(const String& path, String& error) {
    File file = FFat.open(path, FILE_READ);
    if (!file) {
        error = "Временный файл расчёта не найден";
        return false;
    }

    if (path.indexOf(".bin.") >= 0) {
        const bool valid = file.size() == AppConfig::MATRIX_BYTES;
        file.close();
        if (!valid) error = "Бинарная матрица должна иметь размер 131072 байта";
        return valid;
    }

    const size_t size = file.size();
    if (size == 0 || size > AppConfig::MAX_CALCULATION_JSON_BYTES) {
        file.close();
        error = "JSON-расчёт пуст или превышает допустимый размер";
        return false;
    }

    JsonDocument document;
    const DeserializationError jsonError = deserializeJson(document, file);
    file.close();
    if (jsonError) {
        error = "Некорректный JSON-расчёт: " + String(jsonError.c_str());
        return false;
    }
    if (!document.is<JsonObject>()) {
        error = "Корнем JSON-расчёта должен быть объект";
        return false;
    }
    return true;
}

bool StorageManager::resolveDataFile(const String& type,
                                     const String& fileName,
                                     String& path,
                                     String& contentType,
                                     String& error) {
    if (!safeFileName(fileName)) {
        error = "Недопустимое имя файла";
        return false;
    }

    if (type == "reference") {
        if (!fileName.endsWith(".ref")) {
            error = "Неверный тип файла эталона";
            return false;
        }
        path = String(AppConfig::REFERENCE_DIR) + "/" + fileName;
    } else if (type == "calculation" || type == "result") {
        if (!fileName.endsWith(".json") && !fileName.endsWith(".bin")) {
            error = "Неверный тип файла расчёта";
            return false;
        }
        path = String(AppConfig::RESULT_DIR) + "/" + fileName;
    } else {
        error = "Неизвестный тип файла";
        return false;
    }

    if (!FFat.exists(path)) {
        error = "Файл не найден";
        return false;
    }
    contentType = mimeTypeFor(path);
    return true;
}

String StorageManager::mimeTypeFor(const String& path) {
    if (path.endsWith(".html")) return "text/html; charset=utf-8";
    if (path.endsWith(".css")) return "text/css; charset=utf-8";
    if (path.endsWith(".js")) return "application/javascript; charset=utf-8";
    if (path.endsWith(".json")) return "application/json; charset=utf-8";
    if (path.endsWith(".svg")) return "image/svg+xml";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".ico")) return "image/x-icon";
    if (path.endsWith(".ref")) return "application/vnd.cable-tester.reference";
    if (path.endsWith(".bin")) return "application/octet-stream";
    return "application/octet-stream";
}

bool StorageManager::openStaticFile(const String& path, File& file, String& contentType) {
    String safePath = path;
    if (safePath == "/") safePath = "/index.html";
    if (safePath.indexOf("..") >= 0) return false;
    file = LittleFS.open(safePath, FILE_READ);
    if (!file || file.isDirectory()) return false;
    contentType = mimeTypeFor(safePath);
    return true;
}

bool StorageManager::openDataFile(const String& relativePath, File& file, String& contentType) {
    if (relativePath.indexOf("..") >= 0 || !relativePath.startsWith("/")) return false;
    file = FFat.open(relativePath, FILE_READ);
    if (!file || file.isDirectory()) return false;
    contentType = mimeTypeFor(relativePath);
    return true;
}

uint32_t StorageManager::nextResultSequence() {
    constexpr const char* path = "/result-sequence.txt";
    uint32_t value = 0;
    File input = FFat.open(path, FILE_READ);
    if (input) {
        value = static_cast<uint32_t>(input.parseInt());
        input.close();
    }
    ++value;
    File output = FFat.open(path, FILE_WRITE);
    if (output) {
        output.print(value);
        output.flush();
        output.close();
    }
    return value;
}

uint32_t StorageManager::crc32Update(uint32_t crc, const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
        }
    }
    return crc;
}

uint32_t StorageManager::crc32(const uint8_t* data, size_t length) {
    return ~crc32Update(0xFFFFFFFFUL, data, length);
}

String StorageManager::sanitizeBaseName(const String& value) {
    String result;
    result.reserve(64);
    for (size_t i = 0; i < value.length() && result.length() < 60; ++i) {
        const char c = value[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            result += c;
        } else if (c == ' ' || c == '.') {
            if (!result.endsWith("-")) result += '-';
        }
    }
    while (result.endsWith("-")) result.remove(result.length() - 1);
    if (result.isEmpty()) result = "unnamed";
    return result;
}

bool StorageManager::safeFileName(const String& value) {
    return !value.isEmpty() && value.indexOf('/') < 0 && value.indexOf('\\') < 0 && value.indexOf("..") < 0;
}

String StorageManager::baseFileName(const String& value) {
    String result = value;
    const int forwardSlash = result.lastIndexOf('/');
    const int backwardSlash = result.lastIndexOf('\\');
    const int slash = forwardSlash > backwardSlash ? forwardSlash : backwardSlash;
    if (slash >= 0) result = result.substring(slash + 1);
    return result;
}

void StorageManager::cleanupUploadTemps(const char* directory) {
    File dir = FFat.open(directory);
    if (!dir || !dir.isDirectory()) return;

    File file = dir.openNextFile();
    while (file) {
        const String path = file.name();
        const bool remove = !file.isDirectory() && path.endsWith(".upload");
        file.close();
        if (remove) FFat.remove(path);
        file = dir.openNextFile();
    }
}
