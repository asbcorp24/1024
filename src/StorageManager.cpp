#include "StorageManager.h"

#include <LittleFS.h>
#include <FFat.h>
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
        error = "Не удалось создать каталог результатов";
        return false;
    }
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

String StorageManager::listDirectoryJson(const char* directory, const char* extension) {
    String json = "[";
    bool first = true;
    File dir = FFat.open(directory);
    if (!dir || !dir.isDirectory()) {
        return "[]";
    }

    File file = dir.openNextFile();
    while (file) {
        String fullName = file.name();
        String fileName = fullName;
        const int slash = fileName.lastIndexOf('/');
        if (slash >= 0) fileName = fileName.substring(slash + 1);

        if (!file.isDirectory() && fileName.endsWith(extension)) {
            if (!first) json += ',';
            first = false;
            json += "{\"file\":\"" + jsonEscape(fileName) + "\",";
            json += "\"size\":" + String(static_cast<uint32_t>(file.size())) + "}";
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

String StorageManager::mimeTypeFor(const String& path) {
    if (path.endsWith(".html")) return "text/html; charset=utf-8";
    if (path.endsWith(".css")) return "text/css; charset=utf-8";
    if (path.endsWith(".js")) return "application/javascript; charset=utf-8";
    if (path.endsWith(".json")) return "application/json; charset=utf-8";
    if (path.endsWith(".svg")) return "image/svg+xml";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".ico")) return "image/x-icon";
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

uint32_t StorageManager::crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
        }
    }
    return ~crc;
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
