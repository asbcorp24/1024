#pragma once

#include <Arduino.h>
#include "AppConfig.h"

enum class TestMode : uint8_t {
    Idle,
    CaptureReference,
    Compare
};

enum class TestState : uint8_t {
    Idle,
    Preparing,
    Scanning,
    Saving,
    Completed,
    Failed
};

struct __attribute__((packed)) ReferenceFileHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t pinCount;
    uint16_t rowBytes;
    uint16_t reserved;
    uint32_t matrixBytes;
    uint32_t matrixCrc32;
    uint32_t mappingCrc32;
    uint64_t createdAtMs;
    char name[64];
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
        case TestState::Saving: return "saving";
        case TestState::Completed: return "completed";
        case TestState::Failed: return "failed";
        default: return "idle";
    }
}
