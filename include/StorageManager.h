#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "TestTypes.h"

struct sqlite3;

class StorageManager {
public:
    ~StorageManager();
    bool begin(String& error);

    String listReferencesJson();
    String listResultsJson();
    String listCalculationsJson(const String& dateFrom = "",
                                const String& dateTo = "",
                                const String& deviceId = "",
                                const String& operatorName = "");

    bool saveReference(const ReferenceCaptureMetadata& metadata,
                       const uint8_t* matrix,
                       String& savedFile,
                       String& error);

    bool loadReference(const String& fileName,
                       uint8_t* matrix,
                       ReferenceFileHeader& header,
                       String* annotationsJson,
                       String& error);

    bool loadReference(const String& fileName,
                       uint8_t* matrix,
                       ReferenceFileHeader& header,
                       String& error) {
        return loadReference(fileName, matrix, header, nullptr, error);
    }

    bool loadMeasurement(const String& fileName,
                         uint8_t* matrix,
                         String& error);

    bool saveMeasurement(const String& baseName,
                         const uint8_t* matrix,
                         String& savedFile,
                         String& error);

    bool saveResultReport(const String& baseName,
                          const String& json,
                          String& savedFile,
                          String& error);

    bool readResult(const String& fileName, String& json, String& error);
    bool packMatrixToBase64(const uint8_t* matrix,
                           String& encoded,
                           uint32_t& crc32Value,
                           String& error);
    bool unpackMatrixFromBase64(const String& encoded,
                                uint8_t* matrix,
                                uint32_t expectedCrc32,
                                String& error);
    bool deleteReference(const String& fileName, String& error);
    bool deleteCalculation(const String& fileName, String& error);
    bool openStaticFile(const String& path, File& file, String& contentType);
    bool openDataFile(const String& relativePath, File& file, String& contentType);
    uint64_t totalBytes() const;
    uint64_t usedBytes() const;

    bool prepareUpload(const String& type,
                       const String& originalFileName,
                       String& tempPath,
                       String& finalPath,
                       String& error);
    bool finalizeUpload(const String& type,
                        const String& tempPath,
                        const String& finalPath,
                        String& savedFile,
                        String& error);
    void discardUpload(const String& tempPath);

    bool resolveDataFile(const String& type,
                         const String& fileName,
                         String& path,
                         String& contentType,
                         String& error);

    uint32_t nextResultSequence();

    static uint32_t crc32(const uint8_t* data, size_t length);
    static String sanitizeBaseName(const String& value);
    static bool safeFileName(const String& value);

private:
    bool staticMounted_ = false;
    bool dataMounted_ = false;
    sqlite3* metadataDb_ = nullptr;
    SemaphoreHandle_t metadataMutex_ = nullptr;

    String listReferencesJsonLegacy();
    String listCalculationsJsonLegacy();
    String listDirectoryJson(const char* directory,
                             const char* extensionA,
                             const char* extensionB = nullptr);
    String listReferencesJsonFromDb();
    String listCalculationsJsonFromDb(const String& dateFrom,
                                      const String& dateTo,
                                      const String& deviceId,
                                      const String& operatorName);
    bool validateReferenceFile(const String& path, String& error);
    bool validateCalculationFile(const String& path, String& error);
    bool initializeMetadataDb(String& error);
    bool lockMetadata(uint32_t timeoutMs = portMAX_DELAY) const;
    void unlockMetadata() const;
    bool executeSql(const char* sql, String& error);
    bool rebuildMetadataDb(String& error);
    bool upsertReferenceMetadata(const String& fileName,
                                 const ReferenceFileHeader& header,
                                 size_t fileSize,
                                 bool valid,
                                 String& error);
    bool upsertReferenceMetadataFromFile(const String& fileName, String& error);
    bool upsertCalculationMetadata(const String& fileName,
                                   const String& reportJson,
                                   size_t fileSize,
                                   String& error);
    bool upsertCalculationMetadataFromFile(const String& fileName, String& error);
    void syncMetadataDb();
    void removeMetadataRow(const char* table, const String& fileName);
    void cleanupUploadTemps(const char* directory);
    void migrateCompressedFiles();

    static uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length);
    static String baseFileName(const String& value);
    static String jsonEscape(const String& value);
    static String mimeTypeFor(const String& path);
};
