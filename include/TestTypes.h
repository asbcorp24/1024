#pragma once

#include <Arduino.h>

#include "AppConfig.h"
#include "BrowserTime.h"

enum class TestMode : uint8_t {
    Idle,
    CaptureReference,
    Compare
};

enum class TestState : uint8_t {
    Idle,
    Preparing,
    Scanning,
    Analyzing,
    Saving,
    Completed,
    Failed
};

struct ReferenceCaptureMetadata {
    BrowserTimeContext time;
    uint32_t mappingCrc32 = 0;
    char name[64] = {};
    char cableType[64] = {};
    char revision[32] = {};
    char deviceId[64] = {};
    char operatorName[96] = {};
    char comment[256] = {};
    char approvalStatus[24] = {};

    bool valid() const {
        return time.valid() && name[0] != '\0' && cableType[0] != '\0' &&
               revision[0] != '\0' && deviceId[0] != '\0' &&
               operatorName[0] != '\0' && approvalStatus[0] != '\0';
    }
};

struct ComparisonRunMetadata {
    BrowserTimeContext time;
    char cableType[64] = {};
    char deviceId[64] = {};
    char operatorName[96] = {};

    bool valid() const {
        return time.valid() &&
               cableType[0] != '\0' &&
               deviceId[0] != '\0' &&
               operatorName[0] != '\0';
    }
};

struct __attribute__((packed)) ReferenceFileHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t headerBytes;
    uint16_t pinCount;
    uint16_t rowBytes;
    uint32_t matrixBytes;
    uint32_t matrixCrc32;
    uint32_t mappingCrc32;
    uint64_t createdAtEpochMs;
    int16_t utcOffsetMinutes;
    uint16_t reserved;
    char name[64];
    char cableType[64];
    char revision[32];
    char deviceId[64];
    char deviceModel[32];
    char firmwareVersion[24];
    char operatorName[96];
    char timeZone[64];
    char comment[256];
    char approvalStatus[24];
};

struct ScanStatistics {
    uint32_t expectedLinks = 0;
    uint32_t measuredLinks = 0;
    uint32_t missingLinks = 0;
    uint32_t extraLinks = 0;
    uint32_t asymmetricLinks = 0;
    uint32_t stuckLowPins = 0;
    uint32_t sourceDriveErrors = 0;
    uint32_t i2cErrors = 0;
};

struct EngineSnapshot {
    TestMode mode = TestMode::Idle;
    TestState state = TestState::Idle;
    uint16_t currentSource = 0;
    uint16_t totalSources = AppConfig::PIN_COUNT;
    uint8_t progressPercent = 0;
    uint32_t elapsedMs = 0;
    ScanStatistics statistics;
    char activeReference[64] = {};
    char lastResultFile[96] = {};
    char message[160] = {};
};

inline const char* testModeName(TestMode mode) {
    switch (mode) {
        case TestMode::CaptureReference: return "capture_reference";
        case TestMode::Compare: return "compare";
        default: return "idle";
    }
}

inline const char* testStateName(TestState state) {
    switch (state) {
        case TestState::Preparing: return "preparing";
        case TestState::Scanning: return "scanning";
        case TestState::Analyzing: return "analyzing";
        case TestState::Saving: return "saving";
        case TestState::Completed: return "completed";
        case TestState::Failed: return "failed";
        default: return "idle";
    }
}
