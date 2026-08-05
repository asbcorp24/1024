#include "TestEngine.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <cstring>

namespace {

String uint64ToString(uint64_t value) {
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    return String(buffer);
}

} // namespace

TestEngine::TestEngine(McpMatrix& matrix, StorageManager& storage)
    : matrix_(matrix), storage_(storage) {}

void TestEngine::begin(bool hardwareReady, const String& hardwareMessage) {
    if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
    hardwareReady_ = hardwareReady;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    snapshot_ = EngineSnapshot{};
    strlcpy(snapshot_.message,
            hardwareReady ? "Система готова" : hardwareMessage.c_str(),
            sizeof(snapshot_.message));
    if (!hardwareReady) snapshot_.state = TestState::Failed;
    xSemaphoreGive(mutex_);
}

bool TestEngine::stateIsBusy(TestState state) {
    return state == TestState::Preparing ||
           state == TestState::Scanning ||
           state == TestState::Analyzing ||
           state == TestState::Saving;
}

bool TestEngine::isBusy() const {
    if (mutex_ == nullptr) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool busy = stateIsBusy(snapshot_.state);
    xSemaphoreGive(mutex_);
    return busy;
}

bool TestEngine::startReferenceCapture(const ReferenceCaptureMetadata& metadata, String& error) {
    if (!hardwareReady_) {
        error = "Аппаратная матрица MCP23017 не готова";
        return false;
    }
    if (!metadata.valid()) {
        error = "Не заполнены обязательные метаданные эталона";
        return false;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (stateIsBusy(snapshot_.state)) {
        xSemaphoreGive(mutex_);
        error = "Уже выполняется другое измерение";
        return false;
    }

    requestedMode_ = TestMode::CaptureReference;
    requestedValue_ = metadata.name;
    requestedTime_ = metadata.time;
    requestedReferenceMetadata_ = metadata;
    snapshot_ = EngineSnapshot{};
    snapshot_.mode = requestedMode_;
    snapshot_.state = TestState::Preparing;
    strlcpy(snapshot_.activeReference, requestedValue_.c_str(), sizeof(snapshot_.activeReference));
    strlcpy(snapshot_.message, "Подготовка эталонного измерения", sizeof(snapshot_.message));
    xSemaphoreGive(mutex_);

    if (xTaskCreatePinnedToCore(taskEntry, "cable-test", 12288, this, 2, &taskHandle_, 0) != pdPASS) {
        taskHandle_ = nullptr;
        setState(TestState::Failed, "Не удалось создать задачу измерения");
        error = "Не удалось создать задачу FreeRTOS";
        return false;
    }
    return true;
}

bool TestEngine::startComparison(const String& referenceFile,
                                 const ComparisonRunMetadata& metadata,
                                 String& error) {
    if (!hardwareReady_) {
        error = "Аппаратная матрица MCP23017 не готова";
        return false;
    }
    if (!metadata.valid()) {
        error = "Не переданы корректные дата и время браузера";
        return false;
    }
    if (!StorageManager::safeFileName(referenceFile) || !referenceFile.endsWith(".ref")) {
        error = "Выбран некорректный файл эталона";
        return false;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (stateIsBusy(snapshot_.state)) {
        xSemaphoreGive(mutex_);
        error = "Уже выполняется другое измерение";
        return false;
    }

    requestedMode_ = TestMode::Compare;
    requestedValue_ = referenceFile;
    requestedTime_ = metadata.time;
    requestedComparisonMetadata_ = metadata;
    requestedReferenceMetadata_ = ReferenceCaptureMetadata{};
    snapshot_ = EngineSnapshot{};
    snapshot_.mode = requestedMode_;
    snapshot_.state = TestState::Preparing;
    strlcpy(snapshot_.activeReference, requestedValue_.c_str(), sizeof(snapshot_.activeReference));
    strlcpy(snapshot_.message, "Загрузка эталона", sizeof(snapshot_.message));
    xSemaphoreGive(mutex_);

    if (xTaskCreatePinnedToCore(taskEntry, "cable-test", 12288, this, 2, &taskHandle_, 0) != pdPASS) {
        taskHandle_ = nullptr;
        setState(TestState::Failed, "Не удалось создать задачу измерения");
        error = "Не удалось создать задачу FreeRTOS";
        return false;
    }
    return true;
}

bool TestEngine::scanSingleSource(uint16_t sourcePin, String& json, String& error) {
    if (!hardwareReady_) {
        error = "Аппаратная матрица MCP23017 не готова";
        return false;
    }
    if (sourcePin >= AppConfig::PIN_COUNT) {
        error = "Недопустимый номер тестового пина";
        return false;
    }
    if (isBusy()) {
        error = "Сейчас выполняется другое измерение";
        return false;
    }

    auto* baseline = static_cast<uint8_t*>(calloc(AppConfig::ROW_BYTES, 1));
    auto* row = static_cast<uint8_t*>(calloc(AppConfig::ROW_BYTES, 1));
    if (baseline == nullptr || row == nullptr) {
        if (baseline) free(baseline);
        if (row) free(row);
        error = "Недостаточно памяти для ручной проверки пина";
        return false;
    }

    if (!matrix_.setAllInputs(error)) {
        free(baseline);
        free(row);
        return false;
    }
    delay(2);

    if (!readRowWithRetry(baseline, error)) {
        free(baseline);
        free(row);
        return false;
    }

    if (bitAtRow(baseline, sourcePin)) {
        free(baseline);
        free(row);
        error = "Исходный пин уже в LOW до начала проверки";
        return false;
    }

    if (!matrix_.drivePinLow(sourcePin, error)) {
        free(baseline);
        free(row);
        return false;
    }
    delayMicroseconds(AppConfig::SOURCE_SETTLE_US);

    if (!readRowWithRetry(row, error)) {
        String releaseError;
        matrix_.releasePin(sourcePin, releaseError);
        free(baseline);
        free(row);
        return false;
    }

    const bool sourceSeenLow = bitAtRow(row, sourcePin);
    for (size_t i = 0; i < AppConfig::ROW_BYTES; ++i) {
        row[i] &= static_cast<uint8_t>(~baseline[i]);
    }
    clearBitInRow(row, sourcePin);

    String releaseError;
    if (!matrix_.releasePin(sourcePin, releaseError)) {
        free(baseline);
        free(row);
        error = releaseError;
        return false;
    }
    if (!matrix_.waitPinHigh(sourcePin, AppConfig::RELEASE_TIMEOUT_US, releaseError)) {
        String resetError;
        matrix_.setAllInputs(resetError);
        free(baseline);
        free(row);
        error = releaseError;
        return false;
    }

    JsonDocument doc;
    doc["ok"] = true;
    doc["sourcePin"] = sourcePin;
    doc["sourceName"] = McpMatrix::pinName(sourcePin);
    doc["sourceSeenLow"] = sourceSeenLow;

    JsonArray connected = doc["connectedPins"].to<JsonArray>();
    uint16_t connectedCount = 0;
    for (uint16_t target = 0; target < AppConfig::PIN_COUNT; ++target) {
        if (!bitAtRow(row, target)) continue;
        JsonObject item = connected.add<JsonObject>();
        item["pin"] = target;
        item["name"] = McpMatrix::pinName(target);
        ++connectedCount;
    }
    doc["connectedCount"] = connectedCount;

    JsonArray stuckLow = doc["baselineLowPins"].to<JsonArray>();
    for (uint16_t pin = 0; pin < AppConfig::PIN_COUNT; ++pin) {
        if (!bitAtRow(baseline, pin)) continue;
        JsonObject item = stuckLow.add<JsonObject>();
        item["pin"] = pin;
        item["name"] = McpMatrix::pinName(pin);
    }

    serializeJson(doc, json);
    free(baseline);
    free(row);
    return true;
}

void TestEngine::taskEntry(void* parameter) {
    static_cast<TestEngine*>(parameter)->runTask();
}

uint8_t* TestEngine::allocateMatrix() {
    auto* matrix = static_cast<uint8_t*>(heap_caps_malloc(AppConfig::MATRIX_BYTES,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (matrix == nullptr) {
        matrix = static_cast<uint8_t*>(heap_caps_malloc(AppConfig::MATRIX_BYTES, MALLOC_CAP_8BIT));
    }
    if (matrix != nullptr) memset(matrix, 0, AppConfig::MATRIX_BYTES);
    return matrix;
}

void TestEngine::runTask() {
    const uint32_t startedMs = millis();
    String error;
    uint8_t* measured = allocateMatrix();
    uint8_t* reference = nullptr;
    auto* baseline = static_cast<uint8_t*>(calloc(AppConfig::ROW_BYTES, 1));

    if (measured == nullptr || baseline == nullptr) {
        setState(TestState::Failed, "Недостаточно памяти для матрицы 1024×1024");
        if (measured) free(measured);
        if (baseline) free(baseline);
        finishTask();
        return;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    const TestMode mode = requestedMode_;
    const String requestedValue = requestedValue_;
    const BrowserTimeContext browserTime = requestedTime_;
    const ReferenceCaptureMetadata captureMetadata = requestedReferenceMetadata_;
    const ComparisonRunMetadata comparisonMetadata = requestedComparisonMetadata_;
    xSemaphoreGive(mutex_);

    ReferenceFileHeader referenceHeader{};
    if (mode == TestMode::Compare) {
        reference = allocateMatrix();
        if (reference == nullptr) {
            setState(TestState::Failed, "Недостаточно памяти для загрузки эталона");
            free(measured);
            free(baseline);
            finishTask();
            return;
        }
        if (!storage_.loadReference(requestedValue, reference, referenceHeader, error)) {
            setState(TestState::Failed, error);
            free(reference);
            free(measured);
            free(baseline);
            finishTask();
            return;
        }
    }

    setState(TestState::Preparing, "Все 1024 линии переводятся во вход с подтяжкой");
    if (!matrix_.setAllInputs(error)) {
        setState(TestState::Failed, error);
        if (reference) free(reference);
        free(measured);
        free(baseline);
        finishTask();
        return;
    }
    delay(2);

    if (!readRowWithRetry(baseline, error)) {
        setState(TestState::Failed, "Не удалось выполнить фоновое измерение: " + error);
        if (reference) free(reference);
        free(measured);
        free(baseline);
        finishTask();
        return;
    }

    ScanStatistics statistics;
    for (uint16_t pin = 0; pin < AppConfig::PIN_COUNT; ++pin) {
        if (bitAtRow(baseline, pin)) ++statistics.stuckLowPins;
    }
    setStatistics(statistics);

    setState(TestState::Scanning, "Выполняется полный перебор 1024 источников");
    if (!runScan(measured, baseline, error)) {
        setState(TestState::Failed, error);
        if (reference) free(reference);
        free(measured);
        free(baseline);
        finishTask();
        return;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    statistics.stuckLowPins = snapshot_.statistics.stuckLowPins;
    statistics.sourceDriveErrors = snapshot_.statistics.sourceDriveErrors;
    statistics.i2cErrors = snapshot_.statistics.i2cErrors;
    xSemaphoreGive(mutex_);

    setState(TestState::Analyzing, "Расчёт отличий и проверка симметрии");
    analyze(reference, measured, statistics);
    setStatistics(statistics);
    setState(TestState::Saving, "Сохранение файлов измерения");

    if (mode == TestMode::CaptureReference) {
        String savedReference;
        if (!storage_.saveReference(captureMetadata, measured, savedReference, error)) {
            setState(TestState::Failed, error);
        } else {
            xSemaphoreTake(mutex_, portMAX_DELAY);
            strlcpy(snapshot_.activeReference, savedReference.c_str(), sizeof(snapshot_.activeReference));
            strlcpy(snapshot_.message, "Эталон v3 успешно сохранён", sizeof(snapshot_.message));
            snapshot_.elapsedMs = millis() - startedMs;
            snapshot_.state = TestState::Completed;
            xSemaphoreGive(mutex_);
        }
    } else {
        const uint32_t sequence = storage_.nextResultSequence();
        char baseBuffer[32];
        snprintf(baseBuffer, sizeof(baseBuffer), "result-%06lu", static_cast<unsigned long>(sequence));
        const String baseName(baseBuffer);
        String reportFile;

        const uint32_t elapsed = millis() - startedMs;
        const String report = buildReport(requestedValue,
                                          &referenceHeader,
                                          reference,
                                          measured,
                                          baseline,
                                          statistics,
                                          elapsed,
                                          comparisonMetadata);
        log_i("TestEngine: result report prepared base=%s jsonSize=%u elapsedMs=%lu",
              baseName.c_str(),
              static_cast<unsigned>(report.length()),
              static_cast<unsigned long>(elapsed));
        if (report.startsWith("{\"ok\":false")) {
            log_e("TestEngine: result report build failed base=%s payload=%s",
                  baseName.c_str(),
                  report.c_str());
            setState(TestState::Failed, "Ne udalos sobrat otchet izmereniya");
        } else {
            if (!storage_.saveResultReport(baseName, report, reportFile, error)) {
                log_e("TestEngine: saveResultReport failed base=%s error=%s",
                      baseName.c_str(),
                      error.c_str());
                setState(TestState::Failed, error);
            } else {
                log_i("TestEngine: saveResultReport ok file=%s", reportFile.c_str());
                xSemaphoreTake(mutex_, portMAX_DELAY);
                strlcpy(snapshot_.lastResultFile, reportFile.c_str(), sizeof(snapshot_.lastResultFile));
                strlcpy(snapshot_.message,
                        (statistics.missingLinks == 0 && statistics.extraLinks == 0 &&
                         statistics.asymmetricLinks == 0 && statistics.sourceDriveErrors == 0)
                            ? "Кабель соответствует эталону"
                            : "Испытание завершено, обнаружены отличия",
                        sizeof(snapshot_.message));
                snapshot_.elapsedMs = elapsed;
                snapshot_.state = TestState::Completed;
                xSemaphoreGive(mutex_);
            }
        }
    }

    if (reference) free(reference);
    free(measured);
    free(baseline);
    finishTask();
}

bool TestEngine::runScan(uint8_t* measured, uint8_t* baseline, String& error) {
    const uint32_t startedMs = millis();
    ScanStatistics runningStats;
    for (uint16_t pin = 0; pin < AppConfig::PIN_COUNT; ++pin) {
        if (bitAtRow(baseline, pin)) ++runningStats.stuckLowPins;
    }

    for (uint16_t source = 0; source < AppConfig::PIN_COUNT; ++source) {
        uint8_t* row = measured + static_cast<size_t>(source) * AppConfig::ROW_BYTES;

        if (bitAtRow(baseline, source)) {
            ++runningStats.sourceDriveErrors;
            memset(row, 0, AppConfig::ROW_BYTES);
            setStatistics(runningStats);
            setProgress(source + 1, millis() - startedMs);
            continue;
        }

        if (!matrix_.drivePinLow(source, error)) {
            ++runningStats.i2cErrors;
            setStatistics(runningStats);
            return false;
        }
        delayMicroseconds(AppConfig::SOURCE_SETTLE_US);

        if (!readRowWithRetry(row, error)) {
            ++runningStats.i2cErrors;
            String releaseError;
            matrix_.releasePin(source, releaseError);
            setStatistics(runningStats);
            return false;
        }

        if (!bitAtRow(row, source)) ++runningStats.sourceDriveErrors;

        for (size_t i = 0; i < AppConfig::ROW_BYTES; ++i) {
            row[i] &= static_cast<uint8_t>(~baseline[i]);
        }
        clearBitInRow(row, source);

        if (!matrix_.releasePin(source, error)) {
            ++runningStats.i2cErrors;
            setStatistics(runningStats);
            return false;
        }
        if (!matrix_.waitPinHigh(source, AppConfig::RELEASE_TIMEOUT_US, error)) {
            ++runningStats.sourceDriveErrors;
            String resetError;
            matrix_.setAllInputs(resetError);
            setStatistics(runningStats);
            return false;
        }

        setStatistics(runningStats);
        setProgress(source + 1, millis() - startedMs);
        taskYIELD();
    }
    return true;
}

bool TestEngine::readRowWithRetry(uint8_t* row, String& error) {
    for (uint8_t attempt = 0; attempt <= AppConfig::UNSTABLE_RETRY_COUNT; ++attempt) {
        if (matrix_.readAllLowMask(row, error)) return true;
        delay(1);
    }
    return false;
}

void TestEngine::analyze(const uint8_t* reference,
                         const uint8_t* measured,
                         ScanStatistics& statistics) {
    statistics.measuredLinks = 0;
    statistics.expectedLinks = 0;
    statistics.missingLinks = 0;
    statistics.extraLinks = 0;
    statistics.asymmetricLinks = 0;

    for (uint16_t a = 0; a < AppConfig::PIN_COUNT; ++a) {
        for (uint16_t b = a + 1; b < AppConfig::PIN_COUNT; ++b) {
            const bool measuredAB = bitAt(measured, a, b);
            const bool measuredBA = bitAt(measured, b, a);
            const bool measuredConnected = measuredAB && measuredBA;
            if (measuredConnected) ++statistics.measuredLinks;
            if (measuredAB != measuredBA) ++statistics.asymmetricLinks;

            if (reference != nullptr) {
                const bool expectedAB = bitAt(reference, a, b);
                const bool expectedBA = bitAt(reference, b, a);
                const bool expectedConnected = expectedAB && expectedBA;
                if (expectedConnected) ++statistics.expectedLinks;
                if (expectedConnected && !measuredConnected) ++statistics.missingLinks;
                if (!expectedConnected && measuredConnected) ++statistics.extraLinks;
            }
        }
    }
}

String TestEngine::buildReport(const String& referenceFile,
                               const ReferenceFileHeader* referenceHeader,
                               const uint8_t* reference,
                               const uint8_t* measured,
                               const uint8_t* baseline,
                               const ScanStatistics& statistics,
                               uint32_t elapsedMs,
                               const ComparisonRunMetadata& metadata) {
    const BrowserTimeContext& browserTime = metadata.time;
    const uint64_t finishedAtEpochMs = browserTime.startedAtEpochMs + elapsedMs;
    String packedMeasurement;
    uint32_t measurementMatrixCrc32 = 0;
    String packError;
    if (!storage_.packMatrixToBase64(measured, packedMeasurement, measurementMatrixCrc32, packError)) {
        return String("{\"ok\":false,\"error\":\"") + jsonEscape(packError) + "\"}";
    }

    String json;
    json.reserve(110000);
    json += "{\"schema\":2";
    json += ",\"reference\":\"" + jsonEscape(referenceFile) + "\"";
    json += ",\"measurementMatrixCrc32\":" + String(measurementMatrixCrc32);
    json += ",\"measurementMatrixPacked\":\"" + packedMeasurement + "\"";
    json += ",\"elapsedMs\":" + String(elapsedMs);

    json += ",\"time\":{";
    json += "\"source\":\"browser\"";
    json += ",\"startedAtEpochMs\":" + uint64ToString(browserTime.startedAtEpochMs);
    json += ",\"finishedAtEpochMs\":" + uint64ToString(finishedAtEpochMs);
    json += ",\"startedAtUtc\":\"" + jsonEscape(BrowserTime::formatUtc(browserTime.startedAtEpochMs)) + "\"";
    json += ",\"finishedAtUtc\":\"" + jsonEscape(BrowserTime::formatUtc(finishedAtEpochMs)) + "\"";
    json += ",\"startedAtLocal\":\"" + jsonEscape(BrowserTime::formatLocal(browserTime.startedAtEpochMs, browserTime.utcOffsetMinutes)) + "\"";
    json += ",\"finishedAtLocal\":\"" + jsonEscape(BrowserTime::formatLocal(finishedAtEpochMs, browserTime.utcOffsetMinutes)) + "\"";
    json += ",\"utcOffsetMinutes\":" + String(browserTime.utcOffsetMinutes);
    json += ",\"timeZone\":\"" + jsonEscape(browserTime.timeZone) + "\"}";

    if (referenceHeader != nullptr) {
        json += ",\"referenceMetadata\":{";
        json += "\"formatVersion\":" + String(referenceHeader->version);
        json += ",\"name\":\"" + jsonEscape(referenceHeader->name) + "\"";
        json += ",\"cableType\":\"" + jsonEscape(referenceHeader->cableType) + "\"";
        json += ",\"revision\":\"" + jsonEscape(referenceHeader->revision) + "\"";
        json += ",\"deviceId\":\"" + jsonEscape(referenceHeader->deviceId) + "\"";
        json += ",\"deviceModel\":\"" + jsonEscape(referenceHeader->deviceModel) + "\"";
        json += ",\"firmwareVersion\":\"" + jsonEscape(referenceHeader->firmwareVersion) + "\"";
        json += ",\"operator\":\"" + jsonEscape(referenceHeader->operatorName) + "\"";
        json += ",\"comment\":\"" + jsonEscape(referenceHeader->comment) + "\"";
        json += ",\"approvalStatus\":\"" + jsonEscape(referenceHeader->approvalStatus) + "\"";
        json += ",\"mappingCrc32\":" + String(referenceHeader->mappingCrc32);
        json += ",\"matrixCrc32\":" + String(referenceHeader->matrixCrc32);
        json += ",\"createdAtEpochMs\":" + uint64ToString(referenceHeader->createdAtEpochMs);
        json += ",\"createdAtUtc\":\"" + jsonEscape(BrowserTime::formatUtc(referenceHeader->createdAtEpochMs)) + "\"";
        json += ",\"createdAtLocal\":\"" + jsonEscape(BrowserTime::formatLocal(referenceHeader->createdAtEpochMs, referenceHeader->utcOffsetMinutes)) + "\"";
        json += ",\"utcOffsetMinutes\":" + String(referenceHeader->utcOffsetMinutes);
        json += ",\"timeZone\":\"" + jsonEscape(referenceHeader->timeZone) + "\"}";
    }

    json += ",\"measurementMetadata\":{";
    json += "\"cableType\":\"" + jsonEscape(metadata.cableType) + "\"";
    json += ",\"deviceId\":\"" + jsonEscape(metadata.deviceId) + "\"";
    json += ",\"operator\":\"" + jsonEscape(metadata.operatorName) + "\"}";

    json += ",\"passed\":";
    json += (statistics.missingLinks == 0 && statistics.extraLinks == 0 &&
             statistics.asymmetricLinks == 0 && statistics.sourceDriveErrors == 0)
                ? "true" : "false";

    json += ",\"summary\":{";
    json += "\"expectedLinks\":" + String(statistics.expectedLinks);
    json += ",\"measuredLinks\":" + String(statistics.measuredLinks);
    json += ",\"missingLinks\":" + String(statistics.missingLinks);
    json += ",\"extraLinks\":" + String(statistics.extraLinks);
    json += ",\"asymmetricLinks\":" + String(statistics.asymmetricLinks);
    json += ",\"stuckLowPins\":" + String(statistics.stuckLowPins);
    json += ",\"sourceDriveErrors\":" + String(statistics.sourceDriveErrors);
    json += ",\"i2cErrors\":" + String(statistics.i2cErrors) + "}";

    json += ",\"stuckLow\":[";
    bool first = true;
    for (uint16_t pin = 0; pin < AppConfig::PIN_COUNT; ++pin) {
        if (!bitAtRow(baseline, pin)) continue;
        if (!first) json += ',';
        first = false;
        json += "{\"pin\":" + String(pin) + ",\"name\":\"" + jsonEscape(McpMatrix::pinName(pin)) + "\"}";
    }
    json += ']';

    auto appendPairs = [&](const char* key, uint8_t mode) {
        json += ",\"";
        json += key;
        json += "\":[";
        size_t emitted = 0;
        bool pairFirst = true;
        for (uint16_t a = 0; a < AppConfig::PIN_COUNT && emitted < MAX_REPORTED_PAIRS; ++a) {
            for (uint16_t b = a + 1; b < AppConfig::PIN_COUNT && emitted < MAX_REPORTED_PAIRS; ++b) {
                const bool measuredConnected = bitAt(measured, a, b) && bitAt(measured, b, a);
                const bool expectedConnected = reference && bitAt(reference, a, b) && bitAt(reference, b, a);
                const bool asymmetric = bitAt(measured, a, b) != bitAt(measured, b, a);
                bool include = false;
                if (mode == 0) include = expectedConnected && !measuredConnected;
                if (mode == 1) include = !expectedConnected && measuredConnected;
                if (mode == 2) include = asymmetric;
                if (!include) continue;

                if (!pairFirst) json += ',';
                pairFirst = false;
                json += "{\"a\":" + String(a) + ",\"b\":" + String(b);
                json += ",\"aName\":\"" + jsonEscape(McpMatrix::pinName(a)) + "\"";
                json += ",\"bName\":\"" + jsonEscape(McpMatrix::pinName(b)) + "\"}";
                ++emitted;
            }
        }
        json += ']';
        json += ",\"";
        json += key;
        json += "Truncated\":";
        const uint32_t total = mode == 0 ? statistics.missingLinks :
                               mode == 1 ? statistics.extraLinks : statistics.asymmetricLinks;
        json += total > emitted ? "true" : "false";
    };

    appendPairs("missing", 0);
    appendPairs("extra", 1);
    appendPairs("asymmetric", 2);
    json += '}';
    return json;
}

void TestEngine::setState(TestState state, const String& message) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    snapshot_.state = state;
    strlcpy(snapshot_.message, message.c_str(), sizeof(snapshot_.message));
    xSemaphoreGive(mutex_);
}

void TestEngine::setProgress(uint16_t currentSource, uint32_t elapsedMs) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    snapshot_.currentSource = currentSource;
    snapshot_.progressPercent = static_cast<uint8_t>((static_cast<uint32_t>(currentSource) * 100U) / AppConfig::PIN_COUNT);
    snapshot_.elapsedMs = elapsedMs;
    xSemaphoreGive(mutex_);
}

void TestEngine::setStatistics(const ScanStatistics& statistics) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    snapshot_.statistics = statistics;
    xSemaphoreGive(mutex_);
}

void TestEngine::finishTask() {
    taskHandle_ = nullptr;
    vTaskDelete(nullptr);
}

EngineSnapshot TestEngine::snapshot() {
    EngineSnapshot copy;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    copy = snapshot_;
    xSemaphoreGive(mutex_);
    return copy;
}

String TestEngine::statusJson() {
    const EngineSnapshot s = snapshot();
    JsonDocument doc;
    doc["mode"] = testModeName(s.mode);
    doc["state"] = testStateName(s.state);
    doc["currentSource"] = s.currentSource;
    doc["totalSources"] = s.totalSources;
    doc["progress"] = s.progressPercent;
    doc["elapsedMs"] = s.elapsedMs;
    doc["reference"] = s.activeReference;
    doc["lastResult"] = s.lastResultFile;
    doc["message"] = s.message;

    JsonObject stats = doc["statistics"].to<JsonObject>();
    stats["expectedLinks"] = s.statistics.expectedLinks;
    stats["measuredLinks"] = s.statistics.measuredLinks;
    stats["missingLinks"] = s.statistics.missingLinks;
    stats["extraLinks"] = s.statistics.extraLinks;
    stats["asymmetricLinks"] = s.statistics.asymmetricLinks;
    stats["stuckLowPins"] = s.statistics.stuckLowPins;
    stats["sourceDriveErrors"] = s.statistics.sourceDriveErrors;
    stats["i2cErrors"] = s.statistics.i2cErrors;

    String json;
    serializeJson(doc, json);
    return json;
}

bool TestEngine::bitAt(const uint8_t* matrix, uint16_t row, uint16_t column) {
    const size_t offset = static_cast<size_t>(row) * AppConfig::ROW_BYTES + (column >> 3);
    return (matrix[offset] & (1U << (column & 0x07))) != 0;
}

bool TestEngine::bitAtRow(const uint8_t* row, uint16_t column) {
    return (row[column >> 3] & (1U << (column & 0x07))) != 0;
}

void TestEngine::clearBitInRow(uint8_t* row, uint16_t column) {
    row[column >> 3] &= static_cast<uint8_t>(~(1U << (column & 0x07)));
}

String TestEngine::jsonEscape(const String& value) {
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
