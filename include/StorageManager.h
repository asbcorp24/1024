#pragma once

#include <Arduino.h>
#include <FS.h>

#include "TestTypes.h"

class StorageManager {
public:
    bool begin(String& error);

    String listReferencesJson();
    String listResultsJson();
    String listCalculationsJson();

    bool saveReference(const String& name,
                       const uint8_t* matrix,
                       uint32_t mappingCrc32,
                       String& savedFile,
                       String& error);

    bool loadReference(const String& fileName,
                       uint8_t* matrix,
                       ReferenceFileHeader& header,
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
    bool openStaticFile(const String& path, File& file, String& contentType);
    bool openDataFile(const String& relativePath, File& file, String& contentType);

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

    String listDirectoryJson(const char* directory,
                             const char* extensionA,
                             const char* extensionB = nullptr);
    bool validateReferenceFile(const String& path, String& error);
    bool validateCalculationFile(const String& path, String& error);
    void cleanupUploadTemps(const char* directory);

    static uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length);
    static String baseFileName(const String& value);
    static String jsonEscape(const String& value);
    static String mimeTypeFor(const String& path);
};
