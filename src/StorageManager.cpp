#include "StorageManager.h"

#include <ArduinoJson.h>
#include <FFat.h>
#include <LittleFS.h>
#include <mbedtls/base64.h>
#include <esp32-hal-log.h>
#include <sqlite3.h>
#include <cstring>
#include <memory>

#include "AppConfig.h"
#include "BrowserTime.h"

namespace {

constexpr const char* METADATA_DB_PATH = "/metadata.db";

constexpr uint32_t PACKED_BLOB_MAGIC = 0x314B4350UL; // PCK1

struct __attribute__((packed)) PackedBlobHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t unpackedSize;
    uint32_t packedSize;
    uint32_t unpackedCrc32;
};

String uint64ToString(uint64_t value) {
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    return String(buffer);
}

bool validReferenceHeader(const ReferenceFileHeader& header) {
    return header.magic == AppConfig::REFERENCE_MAGIC &&
           header.version == AppConfig::FILE_FORMAT_VERSION &&
           header.headerBytes == sizeof(ReferenceFileHeader) &&
           header.pinCount == AppConfig::PIN_COUNT &&
           header.rowBytes == AppConfig::ROW_BYTES &&
           header.matrixBytes >= sizeof(PackedBlobHeader) &&
           header.matrixBytes <= AppConfig::MATRIX_BYTES &&
           header.createdAtEpochMs >= 946684800000ULL;
}

bool packRle(const uint8_t* input,
             size_t inputSize,
             String& error,
             std::unique_ptr<uint8_t[]>& output,
             size_t& outputSize) {
    if (input == nullptr) {
        error = "Invalid input buffer";
        return false;
    }

    const size_t capacity = inputSize * 2 + 16;
    output.reset(new (std::nothrow) uint8_t[capacity]);
    if (!output) {
        error = "Not enough memory for compression";
        return false;
    }

    size_t src = 0;
    size_t dst = 0;
    while (src < inputSize) {
        size_t runLength = 1;
        while (src + runLength < inputSize &&
               input[src + runLength] == input[src] &&
               runLength < 128) {
            ++runLength;
        }

        if (runLength >= 4) {
            output[dst++] = static_cast<uint8_t>(0x80U | static_cast<uint8_t>(runLength - 1));
            output[dst++] = input[src];
            src += runLength;
            continue;
        }

        const size_t literalStart = src;
        size_t literalLength = 0;
        while (src < inputSize && literalLength < 128) {
            runLength = 1;
            while (src + runLength < inputSize &&
                   input[src + runLength] == input[src] &&
                   runLength < 128) {
                ++runLength;
            }
            if (runLength >= 4 && literalLength > 0) break;
            src += runLength >= 4 ? 0 : 1;
            ++literalLength;
            if (runLength >= 4) break;
        }

        output[dst++] = static_cast<uint8_t>(literalLength - 1);
        memcpy(output.get() + dst, input + literalStart, literalLength);
        dst += literalLength;
        src = literalStart + literalLength;
    }

    outputSize = dst;
    return true;
}

bool unpackRle(const uint8_t* input,
               size_t inputSize,
               uint8_t* output,
               size_t outputSize,
               String& error) {
    if (input == nullptr || output == nullptr) {
        error = "Invalid buffers for decompression";
        return false;
    }

    size_t src = 0;
    size_t dst = 0;
    while (src < inputSize && dst < outputSize) {
        const uint8_t token = input[src++];
        const size_t count = static_cast<size_t>((token & 0x7FU) + 1U);
        if ((token & 0x80U) != 0) {
            if (src >= inputSize || dst + count > outputSize) {
                error = "Damaged packed block";
                return false;
            }
            memset(output + dst, input[src++], count);
            dst += count;
        } else {
            if (src + count > inputSize || dst + count > outputSize) {
                error = "Damaged packed block";
                return false;
            }
            memcpy(output + dst, input + src, count);
            src += count;
            dst += count;
        }
    }

    if (src != inputSize || dst != outputSize) {
        error = "Unpacked size mismatch";
        return false;
    }
    return true;
}

bool peekPackedHeader(File& file, PackedBlobHeader& header) {
    if (!file.seek(0)) return false;
    const bool ok = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
                    header.magic == PACKED_BLOB_MAGIC &&
                    header.version == 1;
    file.seek(0);
    return ok;
}

bool buildPackedPayload(const uint8_t* input,
                        size_t inputSize,
                        uint32_t crc,
                        String& error,
                        std::unique_ptr<uint8_t[]>& output,
                        size_t& outputSize) {
    std::unique_ptr<uint8_t[]> packedData;
    size_t packedDataSize = 0;
    if (!packRle(input, inputSize, error, packedData, packedDataSize)) return false;

    outputSize = sizeof(PackedBlobHeader) + packedDataSize;

    output.reset(new (std::nothrow) uint8_t[outputSize]);
    if (!output) {
        error = "Not enough memory for packed payload";
        outputSize = 0;
        return false;
    }

    PackedBlobHeader header {};
    header.magic = PACKED_BLOB_MAGIC;
    header.version = 1;
    header.unpackedSize = static_cast<uint32_t>(inputSize);
    header.packedSize = static_cast<uint32_t>(packedDataSize);
    header.unpackedCrc32 = crc;

    memcpy(output.get(), &header, sizeof(header));
    memcpy(output.get() + sizeof(header), packedData.get(), packedDataSize);
    return true;
}

bool readPackedPayload(File& file,
                       uint8_t* output,
                       size_t outputSize,
                       uint32_t expectedCrc,
                       String& error) {
    PackedBlobHeader header {};
    if (!file.seek(0) ||
        file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        error = "Packed header read failed";
        return false;
    }
    if (header.magic != PACKED_BLOB_MAGIC ||
        header.version != 1 ||
        header.unpackedSize != outputSize ||
        file.size() != sizeof(PackedBlobHeader) + header.packedSize) {
        error = "Invalid packed payload format";
        return false;
    }

    std::unique_ptr<uint8_t[]> packed(new (std::nothrow) uint8_t[header.packedSize]);
    if (!packed) {
        error = "Not enough memory for unpacking";
        return false;
    }
    if (file.read(packed.get(), header.packedSize) != header.packedSize) {
        error = "Packed payload truncated";
        return false;
    }
    if (!unpackRle(packed.get(), header.packedSize, output, outputSize, error)) return false;

    const uint32_t crc = StorageManager::crc32(output, outputSize);
    if ((expectedCrc != 0 && crc != expectedCrc) || crc != header.unpackedCrc32) {
        error = "Packed payload CRC mismatch";
        return false;
    }
    return true;
}

bool readPackedPayloadFromBuffer(const uint8_t* blob,
                                 size_t blobSize,
                                 uint8_t* output,
                                 size_t outputSize,
                                 uint32_t expectedCrc,
                                 String& error) {
    if (blob == nullptr || blobSize < sizeof(PackedBlobHeader)) {
        error = "Packed payload buffer is too small";
        return false;
    }

    PackedBlobHeader header {};
    memcpy(&header, blob, sizeof(header));
    if (header.magic != PACKED_BLOB_MAGIC ||
        header.version != 1 ||
        header.unpackedSize != outputSize ||
        blobSize != sizeof(PackedBlobHeader) + header.packedSize) {
        error = "Invalid packed payload buffer";
        return false;
    }

    if (!unpackRle(blob + sizeof(PackedBlobHeader),
                   header.packedSize,
                   output,
                   outputSize,
                   error)) {
        return false;
    }

    const uint32_t crc = StorageManager::crc32(output, outputSize);
    if ((expectedCrc != 0 && crc != expectedCrc) || crc != header.unpackedCrc32) {
        error = "Packed payload buffer CRC mismatch";
        return false;
    }
    return true;
}

bool writeWholeFile(const String& path, const uint8_t* data, size_t size, String& error) {
    FFat.remove(path);
    File file = FFat.open(path, FILE_WRITE);
    if (!file) {
        error = "Could not create file";
        return false;
    }
    const bool ok = file.write(data, size) == size;
    file.flush();
    file.close();
    if (!ok) {
        FFat.remove(path);
        error = "Could not write file completely";
        return false;
    }
    return true;
}

} // namespace

StorageManager::~StorageManager() {
    if (metadataDb_ != nullptr) {
        sqlite3_close(metadataDb_);
        metadataDb_ = nullptr;
    }
    if (metadataMutex_ != nullptr) {
        vSemaphoreDelete(metadataMutex_);
        metadataMutex_ = nullptr;
    }
}

bool StorageManager::lockMetadata(uint32_t timeoutMs) const {
    if (metadataMutex_ == nullptr) return false;
    const TickType_t ticks = timeoutMs == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
    return xSemaphoreTake(metadataMutex_, ticks) == pdTRUE;
}

void StorageManager::unlockMetadata() const {
    if (metadataMutex_ != nullptr) xSemaphoreGive(metadataMutex_);
}

bool StorageManager::executeSql(const char* sql, String& error) {
    if (metadataDb_ == nullptr) {
        error = "Metadata database is not open";
        return false;
    }
    if (!lockMetadata()) {
        error = "Metadata database is busy";
        return false;
    }

    char* sqliteError = nullptr;
    const int rc = sqlite3_exec(metadataDb_, sql, nullptr, nullptr, &sqliteError);
    if (rc == SQLITE_OK) {
        unlockMetadata();
        return true;
    }

    error = sqliteError != nullptr ? String(sqliteError) : String("SQLite exec failed");
    if (sqliteError != nullptr) sqlite3_free(sqliteError);
    unlockMetadata();
    return false;
}

bool StorageManager::rebuildMetadataDb(String& error) {
    log_w("StorageManager: rebuilding metadata database schema");
    return executeSql("DROP TABLE IF EXISTS \"references\";", error) &&
           executeSql("DROP TABLE IF EXISTS reference_files;", error) &&
           executeSql("DROP TABLE IF EXISTS calculations;", error) &&
           executeSql(
               "CREATE TABLE reference_files ("
               "file TEXT PRIMARY KEY,"
               "size INTEGER NOT NULL,"
               "valid INTEGER NOT NULL,"
               "format_version INTEGER NOT NULL,"
               "name TEXT,"
               "cable_type TEXT,"
               "revision TEXT,"
               "device_id TEXT,"
               "device_model TEXT,"
               "firmware_version TEXT,"
               "operator_name TEXT,"
               "approval_status TEXT,"
               "comment TEXT,"
               "mapping_crc32 INTEGER NOT NULL,"
               "matrix_crc32 INTEGER NOT NULL,"
               "created_at_epoch_ms INTEGER NOT NULL,"
               "created_at_utc TEXT,"
               "created_at_local TEXT,"
               "utc_offset_minutes INTEGER NOT NULL,"
               "time_zone TEXT,"
               "updated_at_epoch_ms INTEGER NOT NULL"
               ");",
               error) &&
           executeSql(
               "CREATE TABLE calculations ("
               "file TEXT PRIMARY KEY,"
               "size INTEGER NOT NULL,"
               "kind TEXT NOT NULL,"
               "reference_file TEXT,"
               "reference_name TEXT,"
               "reference_cable_type TEXT,"
               "reference_revision TEXT,"
               "reference_device_id TEXT,"
               "reference_operator TEXT,"
               "reference_approval_status TEXT,"
               "measurement_cable_type TEXT,"
               "measurement_device_id TEXT,"
               "measurement_operator TEXT,"
               "started_at_epoch_ms INTEGER NOT NULL,"
               "started_at_utc TEXT,"
               "started_at_local TEXT,"
               "utc_offset_minutes INTEGER NOT NULL,"
               "time_zone TEXT,"
               "passed INTEGER NOT NULL,"
               "expected_links INTEGER NOT NULL,"
               "measured_links INTEGER NOT NULL,"
               "missing_links INTEGER NOT NULL,"
               "extra_links INTEGER NOT NULL,"
               "asymmetric_links INTEGER NOT NULL,"
               "stuck_low_pins INTEGER NOT NULL,"
               "source_drive_errors INTEGER NOT NULL,"
               "i2c_errors INTEGER NOT NULL,"
               "elapsed_ms INTEGER NOT NULL,"
               "updated_at_epoch_ms INTEGER NOT NULL"
               ");",
               error) &&
           executeSql("PRAGMA user_version=2;", error);
}

bool StorageManager::initializeMetadataDb(String& error) {
    if (metadataDb_ != nullptr) return true;

    if (metadataMutex_ == nullptr) {
        metadataMutex_ = xSemaphoreCreateMutex();
        if (metadataMutex_ == nullptr) {
            error = "Could not create metadata mutex";
            return false;
        }
    }

    const String path = String(AppConfig::DATA_FS_BASE) + METADATA_DB_PATH;
    if (sqlite3_open(path.c_str(), &metadataDb_) != SQLITE_OK) {
        error = sqlite3_errmsg(metadataDb_);
        if (metadataDb_ != nullptr) {
            sqlite3_close(metadataDb_);
            metadataDb_ = nullptr;
        }
        return false;
    }

    if (!executeSql("PRAGMA journal_mode=WAL;", error) ||
        !executeSql("PRAGMA synchronous=NORMAL;", error) ||
        !executeSql("DROP TABLE IF EXISTS \"references\";", error) ||
        !executeSql(
            "CREATE TABLE IF NOT EXISTS reference_files ("
            "file TEXT PRIMARY KEY,"
            "size INTEGER NOT NULL,"
            "valid INTEGER NOT NULL,"
            "format_version INTEGER NOT NULL,"
            "name TEXT,"
            "cable_type TEXT,"
            "revision TEXT,"
            "device_id TEXT,"
            "device_model TEXT,"
            "firmware_version TEXT,"
            "operator_name TEXT,"
            "approval_status TEXT,"
            "comment TEXT,"
            "mapping_crc32 INTEGER NOT NULL,"
            "matrix_crc32 INTEGER NOT NULL,"
            "created_at_epoch_ms INTEGER NOT NULL,"
            "created_at_utc TEXT,"
            "created_at_local TEXT,"
            "utc_offset_minutes INTEGER NOT NULL,"
            "time_zone TEXT,"
            "updated_at_epoch_ms INTEGER NOT NULL"
            ");",
            error) ||
        !executeSql(
            "CREATE TABLE IF NOT EXISTS calculations ("
            "file TEXT PRIMARY KEY,"
            "size INTEGER NOT NULL,"
            "kind TEXT NOT NULL,"
            "reference_file TEXT,"
            "reference_name TEXT,"
            "reference_cable_type TEXT,"
            "reference_revision TEXT,"
            "reference_device_id TEXT,"
            "reference_operator TEXT,"
            "reference_approval_status TEXT,"
            "measurement_cable_type TEXT,"
            "measurement_device_id TEXT,"
            "measurement_operator TEXT,"
            "started_at_epoch_ms INTEGER NOT NULL,"
            "started_at_utc TEXT,"
            "started_at_local TEXT,"
            "utc_offset_minutes INTEGER NOT NULL,"
            "time_zone TEXT,"
            "passed INTEGER NOT NULL,"
            "expected_links INTEGER NOT NULL,"
            "measured_links INTEGER NOT NULL,"
            "missing_links INTEGER NOT NULL,"
            "extra_links INTEGER NOT NULL,"
            "asymmetric_links INTEGER NOT NULL,"
            "stuck_low_pins INTEGER NOT NULL,"
            "source_drive_errors INTEGER NOT NULL,"
            "i2c_errors INTEGER NOT NULL,"
            "elapsed_ms INTEGER NOT NULL,"
            "updated_at_epoch_ms INTEGER NOT NULL"
            ");",
            error) ||
        !executeSql("PRAGMA user_version=2;", error)) {
        sqlite3_close(metadataDb_);
        metadataDb_ = nullptr;
        return false;
    }

    return true;
}

bool StorageManager::begin(String& error) {
    staticMounted_ = LittleFS.begin(true, AppConfig::STATIC_FS_BASE, 10, "littlefs");
    if (!staticMounted_) {
        error = "LittleFS mount failed";
        return false;
    }

    dataMounted_ = FFat.begin(true, AppConfig::DATA_FS_BASE, 10, "ffat");
    if (!dataMounted_) {
        error = "FFat mount failed";
        return false;
    }

    if (!FFat.exists(AppConfig::REFERENCE_DIR) && !FFat.mkdir(AppConfig::REFERENCE_DIR)) {
        error = "Could not create references directory";
        return false;
    }
    if (!FFat.exists(AppConfig::RESULT_DIR) && !FFat.mkdir(AppConfig::RESULT_DIR)) {
        error = "Could not create results directory";
        return false;
    }

    cleanupUploadTemps(AppConfig::REFERENCE_DIR);
    cleanupUploadTemps(AppConfig::RESULT_DIR);
    if (!initializeMetadataDb(error)) return false;
    migrateCompressedFiles();
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
        const String fileName = baseFileName(file.name());
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

String StorageManager::listReferencesJsonFromDb() {
    if (metadataDb_ == nullptr) return "[]";
    if (!lockMetadata()) return "[]";

    const char* sql =
        "SELECT file,size,valid,format_version,name,cable_type,revision,device_id,device_model,"
        "firmware_version,operator_name,approval_status,comment,mapping_crc32,matrix_crc32,"
        "created_at_epoch_ms,created_at_utc,created_at_local,utc_offset_minutes,time_zone "
        "FROM reference_files ORDER BY created_at_epoch_ms DESC, file ASC;";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(metadataDb_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        unlockMetadata();
        return "[]";
    }

    String json = "[";
    bool first = true;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (!first) json += ',';
        first = false;
        json += "{\"file\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))) + "\"";
        json += ",\"size\":" + String(static_cast<uint32_t>(sqlite3_column_int64(statement, 1)));
        json += ",\"kind\":\"reference\"";
        json += ",\"valid\":" + String(sqlite3_column_int(statement, 2) != 0 ? "true" : "false");
        json += ",\"formatVersion\":" + String(sqlite3_column_int(statement, 3));
        json += ",\"name\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 4) ? sqlite3_column_text(statement, 4) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"cableType\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 5) ? sqlite3_column_text(statement, 5) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"revision\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 6) ? sqlite3_column_text(statement, 6) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"deviceId\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 7) ? sqlite3_column_text(statement, 7) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"deviceModel\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 8) ? sqlite3_column_text(statement, 8) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"firmwareVersion\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 9) ? sqlite3_column_text(statement, 9) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"operator\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 10) ? sqlite3_column_text(statement, 10) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"approvalStatus\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 11) ? sqlite3_column_text(statement, 11) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"comment\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 12) ? sqlite3_column_text(statement, 12) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"mappingCrc32\":" + String(static_cast<uint32_t>(sqlite3_column_int64(statement, 13)));
        json += ",\"matrixCrc32\":" + String(static_cast<uint32_t>(sqlite3_column_int64(statement, 14)));
        json += ",\"createdAtEpochMs\":" + uint64ToString(static_cast<uint64_t>(sqlite3_column_int64(statement, 15)));
        json += ",\"createdAtUtc\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 16) ? sqlite3_column_text(statement, 16) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"createdAtLocal\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 17) ? sqlite3_column_text(statement, 17) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"utcOffsetMinutes\":" + String(sqlite3_column_int(statement, 18));
        json += ",\"timeZone\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 19) ? sqlite3_column_text(statement, 19) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += '}';
    }

    sqlite3_finalize(statement);
    unlockMetadata();
    json += ']';
    return json;
}

String StorageManager::listCalculationsJsonFromDb(const String& dateFrom,
                                                  const String& dateTo,
                                                  const String& deviceId,
                                                  const String& operatorName) {
    if (metadataDb_ == nullptr) return "[]";
    if (!lockMetadata()) return "[]";

    String sql =
        "SELECT file,size,kind,reference_name,measurement_cable_type,measurement_device_id,"
        "measurement_operator,started_at_local,passed,missing_links,extra_links,asymmetric_links "
        "FROM calculations WHERE 1=1";

    if (!dateFrom.isEmpty()) sql += " AND substr(started_at_local,1,10) >= ?1";
    if (!dateTo.isEmpty()) sql += " AND substr(started_at_local,1,10) <= ?" + String(dateFrom.isEmpty() ? 1 : 2);
    const int deviceIndex = 1 + (!dateFrom.isEmpty() ? 1 : 0) + (!dateTo.isEmpty() ? 1 : 0);
    if (!deviceId.isEmpty()) sql += " AND measurement_device_id = ?" + String(deviceIndex);
    const int operatorIndex = deviceIndex + (!deviceId.isEmpty() ? 1 : 0);
    if (!operatorName.isEmpty()) sql += " AND measurement_operator = ?" + String(operatorIndex);
    sql += " ORDER BY started_at_epoch_ms DESC, file ASC;";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(metadataDb_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        unlockMetadata();
        return "[]";
    }

    int bindIndex = 1;
    if (!dateFrom.isEmpty()) sqlite3_bind_text(statement, bindIndex++, dateFrom.c_str(), -1, SQLITE_TRANSIENT);
    if (!dateTo.isEmpty()) sqlite3_bind_text(statement, bindIndex++, dateTo.c_str(), -1, SQLITE_TRANSIENT);
    if (!deviceId.isEmpty()) sqlite3_bind_text(statement, bindIndex++, deviceId.c_str(), -1, SQLITE_TRANSIENT);
    if (!operatorName.isEmpty()) sqlite3_bind_text(statement, bindIndex++, operatorName.c_str(), -1, SQLITE_TRANSIENT);

    String json = "[";
    bool first = true;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (!first) json += ',';
        first = false;
        json += "{\"file\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))) + "\"";
        json += ",\"size\":" + String(static_cast<uint32_t>(sqlite3_column_int64(statement, 1)));
        json += ",\"kind\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 2) ? sqlite3_column_text(statement, 2) : reinterpret_cast<const unsigned char*>("report"))) + "\"";
        json += ",\"name\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 3) ? sqlite3_column_text(statement, 3) : sqlite3_column_text(statement, 0))) + "\"";
        json += ",\"cableType\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 4) ? sqlite3_column_text(statement, 4) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"deviceId\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 5) ? sqlite3_column_text(statement, 5) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"operator\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 6) ? sqlite3_column_text(statement, 6) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"createdAtLocal\":\"" + jsonEscape(reinterpret_cast<const char*>(sqlite3_column_text(statement, 7) ? sqlite3_column_text(statement, 7) : reinterpret_cast<const unsigned char*>(""))) + "\"";
        json += ",\"passed\":" + String(sqlite3_column_int(statement, 8) != 0 ? "true" : "false");
        json += ",\"missingLinks\":" + String(sqlite3_column_int(statement, 9));
        json += ",\"extraLinks\":" + String(sqlite3_column_int(statement, 10));
        json += ",\"asymmetricLinks\":" + String(sqlite3_column_int(statement, 11));
        json += '}';
    }

    sqlite3_finalize(statement);
    unlockMetadata();
    json += ']';
    return json;
}

void StorageManager::removeMetadataRow(const char* table, const String& fileName) {
    if (metadataDb_ == nullptr) return;
    if (!lockMetadata()) return;

    const String sql = String("DELETE FROM ") + table + " WHERE file=?1;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(metadataDb_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        unlockMetadata();
        return;
    }
    sqlite3_bind_text(statement, 1, fileName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    unlockMetadata();
}

bool StorageManager::upsertReferenceMetadata(const String& fileName,
                                             const ReferenceFileHeader& header,
                                             size_t fileSize,
                                             bool valid,
                                             String& error) {
    if (metadataDb_ == nullptr) return false;
    if (!lockMetadata()) {
        error = "Metadata database is busy";
        return false;
    }

    const char* sql =
        "INSERT OR REPLACE INTO reference_files("
        "file,size,valid,format_version,name,cable_type,revision,device_id,device_model,"
        "firmware_version,operator_name,approval_status,comment,mapping_crc32,matrix_crc32,"
        "created_at_epoch_ms,created_at_utc,created_at_local,utc_offset_minutes,time_zone,updated_at_epoch_ms"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(metadataDb_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(metadataDb_);
        unlockMetadata();
        return false;
    }

    const String createdAtUtc = BrowserTime::formatUtc(header.createdAtEpochMs);
    const String createdAtLocal = BrowserTime::formatLocal(header.createdAtEpochMs, header.utcOffsetMinutes);
    const int64_t updatedAt = static_cast<int64_t>(millis());
    sqlite3_bind_text(statement, 1, fileName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(fileSize));
    sqlite3_bind_int(statement, 3, valid ? 1 : 0);
    sqlite3_bind_int(statement, 4, header.version);
    sqlite3_bind_text(statement, 5, header.name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, header.cableType, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 7, header.revision, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 8, header.deviceId, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 9, header.deviceModel, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 10, header.firmwareVersion, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 11, header.operatorName, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 12, header.approvalStatus, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 13, header.comment, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 14, header.mappingCrc32);
    sqlite3_bind_int64(statement, 15, header.matrixCrc32);
    sqlite3_bind_int64(statement, 16, static_cast<sqlite3_int64>(header.createdAtEpochMs));
    sqlite3_bind_text(statement, 17, createdAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 18, createdAtLocal.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 19, header.utcOffsetMinutes);
    sqlite3_bind_text(statement, 20, header.timeZone, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 21, updatedAt);

    const int rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    unlockMetadata();
    if (rc == SQLITE_DONE) return true;

    error = sqlite3_errmsg(metadataDb_);
    return false;
}

bool StorageManager::upsertReferenceMetadataFromFile(const String& fileName, String& error) {
    const String path = String(AppConfig::REFERENCE_DIR) + "/" + fileName;
    File file = FFat.open(path, FILE_READ);
    if (!file) {
        error = "Reference file not found";
        return false;
    }

    const size_t fileSize = file.size();
    ReferenceFileHeader header {};
    const bool readable = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header);
    file.close();
    const bool valid = readable && validReferenceHeader(header) &&
                       fileSize == static_cast<size_t>(header.headerBytes) + header.matrixBytes;

    if (!readable) {
        error = "Reference header read failed";
        return false;
    }
    return upsertReferenceMetadata(fileName, header, fileSize, valid, error);
}

bool StorageManager::upsertCalculationMetadata(const String& fileName,
                                               const String& reportJson,
                                               size_t fileSize,
                                               String& error) {
    if (metadataDb_ == nullptr) return false;
    if (!lockMetadata()) {
        error = "Metadata database is busy";
        return false;
    }

    JsonDocument document;
    const DeserializationError jsonError = deserializeJson(document, reportJson);
    if (jsonError) {
        error = "Calculation JSON is invalid: " + String(jsonError.c_str());
        unlockMetadata();
        return false;
    }

    const JsonObject summary = document["summary"];
    const JsonObject referenceMetadata = document["referenceMetadata"];
    const JsonObject measurementMetadata = document["measurementMetadata"];
    const JsonObject time = document["time"];
    const String referenceFile = document["reference"] | "";
    const String referenceName = referenceMetadata["name"] | "";
    const String referenceCableType = referenceMetadata["cableType"] | "";
    const String referenceRevision = referenceMetadata["revision"] | "";
    const String referenceDeviceId = referenceMetadata["deviceId"] | "";
    const String referenceOperator = referenceMetadata["operator"] | "";
    const String referenceApprovalStatus = referenceMetadata["approvalStatus"] | "";
    const String measurementCableType = measurementMetadata["cableType"] | "";
    const String measurementDeviceId = measurementMetadata["deviceId"] | "";
    const String measurementOperator = measurementMetadata["operator"] | "";
    const String startedAtUtc = time["startedAtUtc"] | "";
    const String startedAtLocal = time["startedAtLocal"] | "";
    const String timeZone = time["timeZone"] | "";

    const char* sql =
        "INSERT OR REPLACE INTO calculations("
        "file,size,kind,reference_file,reference_name,reference_cable_type,reference_revision,"
        "reference_device_id,reference_operator,reference_approval_status,measurement_cable_type,"
        "measurement_device_id,measurement_operator,started_at_epoch_ms,started_at_utc,started_at_local,"
        "utc_offset_minutes,time_zone,passed,expected_links,measured_links,missing_links,extra_links,"
        "asymmetric_links,stuck_low_pins,source_drive_errors,i2c_errors,elapsed_ms,updated_at_epoch_ms"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(metadataDb_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(metadataDb_);
        unlockMetadata();
        return false;
    }

    sqlite3_bind_text(statement, 1, fileName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(fileSize));
    sqlite3_bind_text(statement, 3, "report", -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 4, referenceFile.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 5, referenceName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, referenceCableType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 7, referenceRevision.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 8, referenceDeviceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 9, referenceOperator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 10, referenceApprovalStatus.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 11, measurementCableType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 12, measurementDeviceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 13, measurementOperator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 14, static_cast<sqlite3_int64>(document["time"]["startedAtEpochMs"] | 0ULL));
    sqlite3_bind_text(statement, 15, startedAtUtc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 16, startedAtLocal.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 17, time["utcOffsetMinutes"] | 0);
    sqlite3_bind_text(statement, 18, timeZone.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 19, (document["passed"] | false) ? 1 : 0);
    sqlite3_bind_int(statement, 20, summary["expectedLinks"] | 0);
    sqlite3_bind_int(statement, 21, summary["measuredLinks"] | 0);
    sqlite3_bind_int(statement, 22, summary["missingLinks"] | 0);
    sqlite3_bind_int(statement, 23, summary["extraLinks"] | 0);
    sqlite3_bind_int(statement, 24, summary["asymmetricLinks"] | 0);
    sqlite3_bind_int(statement, 25, summary["stuckLowPins"] | 0);
    sqlite3_bind_int(statement, 26, summary["sourceDriveErrors"] | 0);
    sqlite3_bind_int(statement, 27, summary["i2cErrors"] | 0);
    sqlite3_bind_int(statement, 28, document["elapsedMs"] | 0);
    sqlite3_bind_int64(statement, 29, static_cast<sqlite3_int64>(millis()));

    const int rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    unlockMetadata();
    if (rc == SQLITE_DONE) return true;

    error = sqlite3_errmsg(metadataDb_);
    return false;
}

bool StorageManager::upsertCalculationMetadataFromFile(const String& fileName, String& error) {
    String reportJson;
    if (!readResult(fileName, reportJson, error)) return false;

    File file = FFat.open(String(AppConfig::RESULT_DIR) + "/" + fileName, FILE_READ);
    if (!file) {
        error = "Calculation file not found";
        return false;
    }
    const size_t fileSize = file.size();
    file.close();
    return upsertCalculationMetadata(fileName, reportJson, fileSize, error);
}

void StorageManager::syncMetadataDb() {
    if (metadataDb_ == nullptr) return;

    File referencesDir = FFat.open(AppConfig::REFERENCE_DIR);
    if (referencesDir && referencesDir.isDirectory()) {
        File file = referencesDir.openNextFile();
        while (file) {
            const String fileName = baseFileName(file.name());
            file.close();
            if (fileName.endsWith(".ref")) {
                String error;
                if (!upsertReferenceMetadataFromFile(fileName, error)) {
                    log_w("StorageManager: reference DB sync skipped file=%s error=%s",
                          fileName.c_str(),
                          error.c_str());
                }
            }
            file = referencesDir.openNextFile();
        }
    }

    File resultsDir = FFat.open(AppConfig::RESULT_DIR);
    if (resultsDir && resultsDir.isDirectory()) {
        File file = resultsDir.openNextFile();
        while (file) {
            const String fileName = baseFileName(file.name());
            file.close();
            if (fileName.endsWith(".json")) {
                String error;
                if (!upsertCalculationMetadataFromFile(fileName, error)) {
                    log_w("StorageManager: calculation DB sync skipped file=%s error=%s",
                          fileName.c_str(),
                          error.c_str());
                }
            }
            file = resultsDir.openNextFile();
        }
    }
}

String StorageManager::listReferencesJsonLegacy() {
    String json = "[";
    bool first = true;
    File dir = FFat.open(AppConfig::REFERENCE_DIR);
    if (!dir || !dir.isDirectory()) return "[]";

    File file = dir.openNextFile();
    while (file) {
        const String fileName = baseFileName(file.name());
        if (!file.isDirectory() && fileName.endsWith(".ref")) {
            const size_t fileSize = file.size();
            ReferenceFileHeader header {};
            const bool readable = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header);
            const bool valid = readable && validReferenceHeader(header) &&
                               fileSize == static_cast<size_t>(header.headerBytes) + header.matrixBytes;

            if (!first) json += ',';
            first = false;
            json += "{\"file\":\"" + jsonEscape(fileName) + "\",";
            json += "\"size\":" + String(static_cast<uint32_t>(fileSize));
            json += ",\"kind\":\"reference\"";
            json += ",\"valid\":";
            json += valid ? "true" : "false";

            if (valid) {
                json += ",\"formatVersion\":" + String(header.version);
                json += ",\"name\":\"" + jsonEscape(header.name) + "\"";
                json += ",\"cableType\":\"" + jsonEscape(header.cableType) + "\"";
                json += ",\"revision\":\"" + jsonEscape(header.revision) + "\"";
                json += ",\"deviceId\":\"" + jsonEscape(header.deviceId) + "\"";
                json += ",\"deviceModel\":\"" + jsonEscape(header.deviceModel) + "\"";
                json += ",\"firmwareVersion\":\"" + jsonEscape(header.firmwareVersion) + "\"";
                json += ",\"operator\":\"" + jsonEscape(header.operatorName) + "\"";
                json += ",\"approvalStatus\":\"" + jsonEscape(header.approvalStatus) + "\"";
                json += ",\"comment\":\"" + jsonEscape(header.comment) + "\"";
                json += ",\"mappingCrc32\":" + String(header.mappingCrc32);
                json += ",\"matrixCrc32\":" + String(header.matrixCrc32);
                json += ",\"createdAtEpochMs\":" + uint64ToString(header.createdAtEpochMs);
                json += ",\"createdAtUtc\":\"" + jsonEscape(BrowserTime::formatUtc(header.createdAtEpochMs)) + "\"";
                json += ",\"createdAtLocal\":\"" + jsonEscape(BrowserTime::formatLocal(header.createdAtEpochMs, header.utcOffsetMinutes)) + "\"";
                json += ",\"utcOffsetMinutes\":" + String(header.utcOffsetMinutes);
                json += ",\"timeZone\":\"" + jsonEscape(header.timeZone) + "\"";
            }
            json += '}';
        }
        file = dir.openNextFile();
    }
    json += ']';
    return json;
}

String StorageManager::listCalculationsJsonLegacy() {
    String json = "[";
    bool first = true;
    File dir = FFat.open(AppConfig::RESULT_DIR);
    if (!dir || !dir.isDirectory()) return "[]";

    File file = dir.openNextFile();
    while (file) {
        const String fileName = baseFileName(file.name());
        if (!file.isDirectory() && fileName.endsWith(".json")) {
            String reportJson;
            String error;
            if (readResult(fileName, reportJson, error)) {
                JsonDocument document;
                const DeserializationError jsonError = deserializeJson(document, reportJson);
                if (!jsonError && document.is<JsonObject>()) {
                    const JsonObject summary = document["summary"];
                    const JsonObject measurementMetadata = document["measurementMetadata"];
                    const JsonObject time = document["time"];

                    if (!first) json += ',';
                    first = false;
                    json += "{\"file\":\"" + jsonEscape(fileName) + "\"";
                    json += ",\"size\":" + String(static_cast<uint32_t>(file.size()));
                    json += ",\"kind\":\"report\"";
                    json += ",\"name\":\"" + jsonEscape(String(document["referenceMetadata"]["name"] | document["reference"] | fileName)) + "\"";
                    json += ",\"cableType\":\"" + jsonEscape(String(measurementMetadata["cableType"] | "")) + "\"";
                    json += ",\"deviceId\":\"" + jsonEscape(String(measurementMetadata["deviceId"] | "")) + "\"";
                    json += ",\"operator\":\"" + jsonEscape(String(measurementMetadata["operator"] | "")) + "\"";
                    json += ",\"createdAtLocal\":\"" + jsonEscape(String(time["startedAtLocal"] | "")) + "\"";
                    json += ",\"passed\":";
                    json += (document["passed"] | false) ? "true" : "false";
                    json += ",\"missingLinks\":" + String(summary["missingLinks"] | 0);
                    json += ",\"extraLinks\":" + String(summary["extraLinks"] | 0);
                    json += ",\"asymmetricLinks\":" + String(summary["asymmetricLinks"] | 0);
                    json += '}';
                }
            }
        }
        file = dir.openNextFile();
    }

    json += ']';
    return json;
}

String StorageManager::listReferencesJson() {
    if (metadataDb_ == nullptr) return listReferencesJsonLegacy();
    return listReferencesJsonFromDb();
}

String StorageManager::listResultsJson() {
    return listCalculationsJson();
}

String StorageManager::listCalculationsJson(const String& dateFrom,
                                            const String& dateTo,
                                            const String& deviceId,
                                            const String& operatorName) {
    if (metadataDb_ == nullptr) {
        const String legacyJson = listCalculationsJsonLegacy();
        log_i("StorageManager: calculations list = %s", legacyJson.c_str());
        return legacyJson;
    }

    const String dbJson = listCalculationsJsonFromDb(dateFrom, dateTo, deviceId, operatorName);
    log_i("StorageManager: calculations list = %s", dbJson.c_str());
    return dbJson;
}

bool StorageManager::saveReference(const ReferenceCaptureMetadata& metadata,
                                   const uint8_t* matrix,
                                   String& savedFile,
                                   String& error) {
    if (!dataMounted_ || matrix == nullptr) {
        error = "Storage is not ready";
        return false;
    }
    if (!metadata.valid()) {
        error = "Reference metadata is incomplete";
        return false;
    }

    const uint32_t matrixCrc = crc32(matrix, AppConfig::MATRIX_BYTES);
    std::unique_ptr<uint8_t[]> packedMatrix;
    size_t packedMatrixSize = 0;
    if (!buildPackedPayload(matrix,
                            AppConfig::MATRIX_BYTES,
                            matrixCrc,
                            error,
                            packedMatrix,
                            packedMatrixSize)) {
        return false;
    }

    const String base = sanitizeBaseName(metadata.name);
    const String finalPath = String(AppConfig::REFERENCE_DIR) + "/" + base + ".ref";
    const String tempPath = finalPath + ".tmp";

    ReferenceFileHeader header {};
    header.magic = AppConfig::REFERENCE_MAGIC;
    header.version = AppConfig::FILE_FORMAT_VERSION;
    header.headerBytes = sizeof(ReferenceFileHeader);
    header.pinCount = AppConfig::PIN_COUNT;
    header.rowBytes = AppConfig::ROW_BYTES;
    header.matrixBytes = static_cast<uint32_t>(packedMatrixSize);
    header.matrixCrc32 = matrixCrc;
    header.mappingCrc32 = metadata.mappingCrc32;
    header.createdAtEpochMs = metadata.time.startedAtEpochMs;
    header.utcOffsetMinutes = metadata.time.utcOffsetMinutes;
    strlcpy(header.name, metadata.name, sizeof(header.name));
    strlcpy(header.cableType, metadata.cableType, sizeof(header.cableType));
    strlcpy(header.revision, metadata.revision, sizeof(header.revision));
    strlcpy(header.deviceId, metadata.deviceId, sizeof(header.deviceId));
    strlcpy(header.deviceModel, AppConfig::DEVICE_MODEL, sizeof(header.deviceModel));
    strlcpy(header.firmwareVersion, AppConfig::FIRMWARE_VERSION, sizeof(header.firmwareVersion));
    strlcpy(header.operatorName, metadata.operatorName, sizeof(header.operatorName));
    strlcpy(header.timeZone, metadata.time.timeZone, sizeof(header.timeZone));
    strlcpy(header.comment, metadata.comment, sizeof(header.comment));
    strlcpy(header.approvalStatus, metadata.approvalStatus, sizeof(header.approvalStatus));

    FFat.remove(tempPath);
    File file = FFat.open(tempPath, FILE_WRITE);
    if (!file) {
        error = "Could not create temporary reference file";
        return false;
    }

    const bool ok = file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
                    file.write(packedMatrix.get(), packedMatrixSize) == packedMatrixSize;
    file.flush();
    file.close();

    if (!ok) {
        FFat.remove(tempPath);
        error = "Could not write reference file";
        return false;
    }

    FFat.remove(finalPath);
    if (!FFat.rename(tempPath, finalPath)) {
        FFat.remove(tempPath);
        error = "Could not finalize reference file";
        return false;
    }

    savedFile = base + ".ref";
    if (!upsertReferenceMetadata(savedFile,
                                 header,
                                 static_cast<size_t>(header.headerBytes) + header.matrixBytes,
                                 true,
                                 error)) {
        log_w("StorageManager: reference metadata DB update failed file=%s error=%s",
              savedFile.c_str(),
              error.c_str());
        error = "";
    }
    return true;
}

bool StorageManager::loadReference(const String& fileName,
                                   uint8_t* matrix,
                                   ReferenceFileHeader& header,
                                   String& error) {
    if (!safeFileName(fileName) || !fileName.endsWith(".ref")) {
        error = "Invalid reference file name";
        return false;
    }

    const String path = String(AppConfig::REFERENCE_DIR) + "/" + fileName;
    File file = FFat.open(path, FILE_READ);
    if (!file) {
        error = "Reference file not found";
        return false;
    }

    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        file.close();
        error = "Reference header read failed";
        return false;
    }
    if (!validReferenceHeader(header) ||
        file.size() != static_cast<size_t>(header.headerBytes) + header.matrixBytes) {
        file.close();
        error = "Invalid reference format";
        return false;
    }

    if (!file.seek(header.headerBytes)) {
        file.close();
        error = "Reference payload seek failed";
        return false;
    }

    std::unique_ptr<uint8_t[]> packedBytes(new (std::nothrow) uint8_t[header.matrixBytes]);
    if (!packedBytes) {
        file.close();
        error = "Not enough memory for reference";
        return false;
    }
    if (file.read(packedBytes.get(), header.matrixBytes) != header.matrixBytes) {
        file.close();
        error = "Reference payload truncated";
        return false;
    }
    file.close();

    return readPackedPayloadFromBuffer(packedBytes.get(),
                                       header.matrixBytes,
                                       matrix,
                                       AppConfig::MATRIX_BYTES,
                                       header.matrixCrc32,
                                       error);
}

bool StorageManager::loadMeasurement(const String& fileName,
                                     uint8_t* matrix,
                                     String& error) {
    if (matrix == nullptr) {
        error = "Measurement buffer is null";
        return false;
    }
    if (!safeFileName(fileName) || !fileName.endsWith(".bin")) {
        error = "Invalid measurement file name";
        return false;
    }

    const String path = String(AppConfig::RESULT_DIR) + "/" + fileName;
    File file = FFat.open(path, FILE_READ);
    if (!file) {
        error = "Measurement file not found";
        return false;
    }

    const bool ok = readPackedPayload(file, matrix, AppConfig::MATRIX_BYTES, 0, error);
    file.close();
    return ok;
}

bool StorageManager::saveMeasurement(const String& baseName,
                                     const uint8_t* matrix,
                                     String& savedFile,
                                     String& error) {
    const uint32_t matrixCrc = crc32(matrix, AppConfig::MATRIX_BYTES);
    std::unique_ptr<uint8_t[]> packedMatrix;
    size_t packedMatrixSize = 0;
    if (!buildPackedPayload(matrix,
                            AppConfig::MATRIX_BYTES,
                            matrixCrc,
                            error,
                            packedMatrix,
                            packedMatrixSize)) {
        return false;
    }

    const String base = sanitizeBaseName(baseName);
    const String finalPath = String(AppConfig::RESULT_DIR) + "/" + base + ".bin";
    if (!writeWholeFile(finalPath, packedMatrix.get(), packedMatrixSize, error)) return false;
    savedFile = base + ".bin";
    return true;
}

bool StorageManager::saveResultReport(const String& baseName,
                                      const String& json,
                                      String& savedFile,
                                      String& error) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(json.c_str());
    const size_t size = json.length();
    const uint32_t crc = crc32(bytes, size);
    log_i("StorageManager: saveResultReport start base=%s jsonSize=%u crc32=%08lx",
          baseName.c_str(),
          static_cast<unsigned>(size),
          static_cast<unsigned long>(crc));
    std::unique_ptr<uint8_t[]> packedJson;
    size_t packedJsonSize = 0;
    if (!buildPackedPayload(bytes, size, crc, error, packedJson, packedJsonSize)) {
        log_e("StorageManager: saveResultReport pack failed base=%s error=%s",
              baseName.c_str(),
              error.c_str());
        return false;
    }

    const String base = sanitizeBaseName(baseName);
    const String finalPath = String(AppConfig::RESULT_DIR) + "/" + base + ".json";
    log_i("StorageManager: saveResultReport writing path=%s packedSize=%u",
          finalPath.c_str(),
          static_cast<unsigned>(packedJsonSize));
    if (!writeWholeFile(finalPath, packedJson.get(), packedJsonSize, error)) {
        log_e("StorageManager: saveResultReport write failed path=%s error=%s",
              finalPath.c_str(),
              error.c_str());
        return false;
    }

    File verifyFile = FFat.open(finalPath, FILE_READ);
    if (!verifyFile) {
        error = "Result file written but reopen failed";
        log_e("StorageManager: saveResultReport reopen failed path=%s", finalPath.c_str());
        return false;
    }
    const size_t storedSize = verifyFile.size();
    verifyFile.close();

    savedFile = base + ".json";
    log_i("StorageManager: saveResultReport ok file=%s storedSize=%u",
          savedFile.c_str(),
          static_cast<unsigned>(storedSize));
    if (!upsertCalculationMetadata(savedFile, json, storedSize, error)) {
        log_w("StorageManager: calculation metadata DB update failed file=%s error=%s",
              savedFile.c_str(),
              error.c_str());
        error = "";
    }
    return true;
}

bool StorageManager::readResult(const String& fileName, String& json, String& error) {
    if (!safeFileName(fileName) || !fileName.endsWith(".json")) {
        error = "Invalid result file name";
        log_e("StorageManager: readResult invalid file name=%s", fileName.c_str());
        return false;
    }

    File file = FFat.open(String(AppConfig::RESULT_DIR) + "/" + fileName, FILE_READ);
    if (!file) {
        error = "Result file not found";
        log_e("StorageManager: readResult file not found=%s", fileName.c_str());
        return false;
    }

    PackedBlobHeader header {};
    if (!peekPackedHeader(file, header)) {
        file.close();
        error = "Invalid packed result format";
        log_e("StorageManager: readResult packed header invalid file=%s", fileName.c_str());
        return false;
    }

    std::unique_ptr<uint8_t[]> unpacked(new (std::nothrow) uint8_t[header.unpackedSize + 1U]);
    if (!unpacked) {
        file.close();
        error = "Not enough memory for result JSON";
        log_e("StorageManager: readResult alloc failed file=%s unpackedSize=%u",
              fileName.c_str(),
              static_cast<unsigned>(header.unpackedSize));
        return false;
    }
    if (!readPackedPayload(file, unpacked.get(), header.unpackedSize, header.unpackedCrc32, error)) {
        file.close();
        log_e("StorageManager: readResult unpack failed file=%s error=%s",
              fileName.c_str(),
              error.c_str());
        return false;
    }
    unpacked[header.unpackedSize] = 0;
    json = String(reinterpret_cast<const char*>(unpacked.get()));
    file.close();
    log_i("StorageManager: readResult ok file=%s jsonSize=%u",
          fileName.c_str(),
          static_cast<unsigned>(json.length()));
    return true;
}

bool StorageManager::packMatrixToBase64(const uint8_t* matrix,
                                        String& encoded,
                                        uint32_t& crc32Value,
                                        String& error) {
    if (matrix == nullptr) {
        error = "Matrix buffer is null";
        return false;
    }

    crc32Value = crc32(matrix, AppConfig::MATRIX_BYTES);

    std::unique_ptr<uint8_t[]> packedMatrix;
    size_t packedMatrixSize = 0;
    if (!buildPackedPayload(matrix,
                            AppConfig::MATRIX_BYTES,
                            crc32Value,
                            error,
                            packedMatrix,
                            packedMatrixSize)) {
        return false;
    }

    const size_t base64Capacity = packedMatrixSize * 4 / 3 + 8;
    std::unique_ptr<unsigned char[]> base64Buffer(new (std::nothrow) unsigned char[base64Capacity]);
    if (!base64Buffer) {
        error = "Not enough memory for base64 matrix";
        return false;
    }

    size_t written = 0;
    const int rc = mbedtls_base64_encode(base64Buffer.get(),
                                         base64Capacity,
                                         &written,
                                         packedMatrix.get(),
                                         packedMatrixSize);
    if (rc != 0) {
        error = "Could not encode matrix to base64";
        return false;
    }

    base64Buffer[written] = 0;
    encoded = String(reinterpret_cast<const char*>(base64Buffer.get()));
    return true;
}

bool StorageManager::unpackMatrixFromBase64(const String& encoded,
                                            uint8_t* matrix,
                                            uint32_t expectedCrc32,
                                            String& error) {
    if (matrix == nullptr) {
        error = "Matrix buffer is null";
        return false;
    }
    if (encoded.isEmpty()) {
        error = "Packed matrix is empty";
        return false;
    }

    const size_t base64Length = encoded.length();
    const size_t packedCapacity = base64Length * 3 / 4 + 4;
    std::unique_ptr<uint8_t[]> packedBuffer(new (std::nothrow) uint8_t[packedCapacity]);
    if (!packedBuffer) {
        error = "Not enough memory for base64 decode";
        return false;
    }

    size_t packedSize = 0;
    const int rc = mbedtls_base64_decode(packedBuffer.get(),
                                         packedCapacity,
                                         &packedSize,
                                         reinterpret_cast<const unsigned char*>(encoded.c_str()),
                                         base64Length);
    if (rc != 0) {
        error = "Could not decode packed matrix";
        return false;
    }

    return readPackedPayloadFromBuffer(packedBuffer.get(),
                                       packedSize,
                                       matrix,
                                       AppConfig::MATRIX_BYTES,
                                       expectedCrc32,
                                       error);
}

bool StorageManager::deleteReference(const String& fileName, String& error) {
    if (!dataMounted_) {
        error = "FFat storage is not ready";
        return false;
    }
    if (!safeFileName(fileName) || !fileName.endsWith(".ref")) {
        error = "Invalid reference file name";
        return false;
    }

    const String path = String(AppConfig::REFERENCE_DIR) + "/" + fileName;
    if (!FFat.exists(path)) {
        error = "Reference file not found";
        return false;
    }
    if (!FFat.remove(path)) {
        error = "Could not delete reference file";
        return false;
    }
    removeMetadataRow("reference_files", fileName);
    return true;
}

bool StorageManager::deleteCalculation(const String& fileName, String& error) {
    if (!dataMounted_) {
        error = "FFat storage is not ready";
        return false;
    }
    if (!safeFileName(fileName) || !fileName.endsWith(".json")) {
        error = "Invalid calculation file name";
        return false;
    }

    const String path = String(AppConfig::RESULT_DIR) + "/" + fileName;
    if (!FFat.exists(path)) {
        error = "Calculation file not found";
        return false;
    }
    if (!FFat.remove(path)) {
        error = "Could not delete calculation file";
        return false;
    }
    removeMetadataRow("calculations", fileName);
    return true;
}

uint64_t StorageManager::totalBytes() const {
    return dataMounted_ ? FFat.totalBytes() : 0;
}

uint64_t StorageManager::usedBytes() const {
    return dataMounted_ ? FFat.usedBytes() : 0;
}

bool StorageManager::prepareUpload(const String& type,
                                   const String& originalFileName,
                                   String& tempPath,
                                   String& finalPath,
                                   String& error) {
    if (!dataMounted_) {
        error = "FFat storage is not ready";
        return false;
    }

    const String fileName = baseFileName(originalFileName);
    if (!safeFileName(fileName)) {
        error = "Invalid upload file name";
        return false;
    }

    String directory;
    if (type == "reference") {
        if (!fileName.endsWith(".ref")) {
            error = "Reference file must have .ref extension";
            return false;
        }
        directory = AppConfig::REFERENCE_DIR;
    } else if (type == "calculation") {
        if (!fileName.endsWith(".json")) {
            error = "Calculation file must have .json extension";
            return false;
        }
        directory = AppConfig::RESULT_DIR;
    } else {
        error = "Unknown upload type";
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
        if (error.isEmpty()) error = "Unknown uploaded file";
        FFat.remove(tempPath);
        return false;
    }

    FFat.remove(finalPath);
    if (!FFat.rename(tempPath, finalPath)) {
        FFat.remove(tempPath);
        error = "Could not move uploaded file into storage";
        return false;
    }

    savedFile = baseFileName(finalPath);
    String syncError;
    if (type == "reference") {
        if (!upsertReferenceMetadataFromFile(savedFile, syncError)) {
            log_w("StorageManager: uploaded reference DB sync failed file=%s error=%s",
                  savedFile.c_str(),
                  syncError.c_str());
        }
    } else if (type == "calculation") {
        if (!upsertCalculationMetadataFromFile(savedFile, syncError)) {
            log_w("StorageManager: uploaded calculation DB sync failed file=%s error=%s",
                  savedFile.c_str(),
                  syncError.c_str());
        }
    }
    return true;
}

void StorageManager::discardUpload(const String& tempPath) {
    if (!tempPath.isEmpty() && tempPath.endsWith(".upload")) FFat.remove(tempPath);
}

bool StorageManager::validateReferenceFile(const String& path, String& error) {
    File file = FFat.open(path, FILE_READ);
    if (!file) {
        error = "Temporary reference file not found";
        return false;
    }

    ReferenceFileHeader header {};
    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        file.close();
        error = "Reference header read failed";
        return false;
    }
    if (!validReferenceHeader(header) ||
        file.size() != static_cast<size_t>(header.headerBytes) + header.matrixBytes) {
        file.close();
        error = "Invalid reference file format";
        return false;
    }
    if (!file.seek(header.headerBytes)) {
        file.close();
        error = "Reference payload seek failed";
        return false;
    }

    std::unique_ptr<uint8_t[]> packedBytes(new (std::nothrow) uint8_t[header.matrixBytes]);
    std::unique_ptr<uint8_t[]> matrix(new (std::nothrow) uint8_t[AppConfig::MATRIX_BYTES]);
    if (!packedBytes || !matrix) {
        file.close();
        error = "Not enough memory to validate reference";
        return false;
    }
    if (file.read(packedBytes.get(), header.matrixBytes) != header.matrixBytes) {
        file.close();
        error = "Reference payload truncated";
        return false;
    }
    file.close();

    return readPackedPayloadFromBuffer(packedBytes.get(),
                                       header.matrixBytes,
                                       matrix.get(),
                                       AppConfig::MATRIX_BYTES,
                                       header.matrixCrc32,
                                       error);
}

bool StorageManager::validateCalculationFile(const String& path, String& error) {
    File file = FFat.open(path, FILE_READ);
    if (!file) {
        error = "Temporary calculation file not found";
        return false;
    }

    PackedBlobHeader header {};
    if (!peekPackedHeader(file, header)) {
        file.close();
        error = "Packed header not found";
        return false;
    }

    if (header.unpackedSize == 0 || header.unpackedSize > AppConfig::MAX_CALCULATION_JSON_BYTES) {
        file.close();
        error = "Packed JSON report size is invalid";
        return false;
    }

    std::unique_ptr<uint8_t[]> unpacked(new (std::nothrow) uint8_t[header.unpackedSize + 1U]);
    if (!unpacked) {
        file.close();
        error = "Not enough memory to validate JSON report";
        return false;
    }
    if (!readPackedPayload(file, unpacked.get(), header.unpackedSize, header.unpackedCrc32, error)) {
        file.close();
        return false;
    }
    unpacked[header.unpackedSize] = 0;
    file.close();

    JsonDocument document;
    const DeserializationError jsonError =
        deserializeJson(document, reinterpret_cast<const char*>(unpacked.get()));
    if (jsonError) {
        error = "JSON report is invalid: " + String(jsonError.c_str());
        return false;
    }
    if (!document.is<JsonObject>()) {
        error = "JSON root must be an object";
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
        error = "Invalid file name";
        return false;
    }

    if (type == "reference") {
        if (!fileName.endsWith(".ref")) {
            error = "Invalid reference file type";
            return false;
        }
        path = String(AppConfig::REFERENCE_DIR) + "/" + fileName;
    } else if (type == "calculation" || type == "result") {
        if (!fileName.endsWith(".json")) {
            error = "Invalid calculation file type";
            return false;
        }
        path = String(AppConfig::RESULT_DIR) + "/" + fileName;
    } else {
        error = "Unknown file type";
        return false;
    }

    if (!FFat.exists(path)) {
        error = "File not found";
        return false;
    }
    contentType = mimeTypeFor(path);
    return true;
}

String StorageManager::mimeTypeFor(const String& path) {
    if (path.endsWith(".html")) return "text/html; charset=utf-8";
    if (path.endsWith(".css")) return "text/css; charset=utf-8";
    if (path.endsWith(".js")) return "application/javascript; charset=utf-8";
    if (path.endsWith(".json")) return "application/octet-stream";
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
            result += '_';
        }
    }
    if (result.isEmpty()) result = "file";
    return result;
}

bool StorageManager::safeFileName(const String& value) {
    if (value.isEmpty() || value.length() > 96) return false;
    if (value.startsWith(".") || value.indexOf("..") >= 0) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

String StorageManager::baseFileName(const String& value) {
    const int slash = value.lastIndexOf('/');
    return slash >= 0 ? value.substring(slash + 1) : value;
}

void StorageManager::cleanupUploadTemps(const char* directory) {
    File dir = FFat.open(directory);
    if (!dir || !dir.isDirectory()) return;

    File file = dir.openNextFile();
    while (file) {
        const String path = file.name();
        if (!file.isDirectory() && path.endsWith(".upload")) FFat.remove(path);
        file = dir.openNextFile();
    }
}

void StorageManager::migrateCompressedFiles() {
}
