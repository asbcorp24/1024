#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "BrowserTime.h"
#include "McpMatrix.h"
#include "StorageManager.h"
#include "TestTypes.h"

class TestEngine {
public:
    TestEngine(McpMatrix& matrix, StorageManager& storage);

    void begin(bool hardwareReady, const String& hardwareMessage);
    bool startReferenceCapture(const ReferenceCaptureMetadata& metadata, String& error);
    bool startComparison(const String& referenceFile,
                         const ComparisonRunMetadata& metadata,
                         String& error);
    bool scanSingleSource(uint16_t sourcePin, String& json, String& error);
    bool isBusy() const;

    EngineSnapshot snapshot();
    String statusJson();

private:
    static constexpr size_t MAX_REPORTED_PAIRS = 1500;

    McpMatrix& matrix_;
    StorageManager& storage_;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t taskHandle_ = nullptr;
    EngineSnapshot snapshot_;
    bool hardwareReady_ = false;
    String requestedValue_;
    TestMode requestedMode_ = TestMode::Idle;
    BrowserTimeContext requestedTime_;
    ReferenceCaptureMetadata requestedReferenceMetadata_;
    ComparisonRunMetadata requestedComparisonMetadata_;

    static void taskEntry(void* parameter);
    void runTask();
    bool runScan(uint8_t* measured, uint8_t* baseline, String& error);
    bool readRowWithRetry(uint8_t* row, String& error);
    void analyze(const uint8_t* reference, const uint8_t* measured, ScanStatistics& statistics);
    String buildReport(const String& referenceFile,
                       const ReferenceFileHeader* referenceHeader,
                       const uint8_t* reference,
                       const uint8_t* measured,
                       const uint8_t* baseline,
                       const ScanStatistics& statistics,
                       uint32_t elapsedMs,
                       const ComparisonRunMetadata& metadata);

    void setState(TestState state, const String& message);
    void setProgress(uint16_t currentSource, uint32_t elapsedMs);
    void setStatistics(const ScanStatistics& statistics);
    void finishTask();

    static bool stateIsBusy(TestState state);
    static bool bitAt(const uint8_t* matrix, uint16_t row, uint16_t column);
    static bool bitAtRow(const uint8_t* row, uint16_t column);
    static void clearBitInRow(uint8_t* row, uint16_t column);
    static uint8_t* allocateMatrix();
    static String jsonEscape(const String& value);
};
