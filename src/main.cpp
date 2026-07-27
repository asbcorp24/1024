#include <Arduino.h>

#include "McpMatrix.h"
#include "StorageManager.h"
#include "TestEngine.h"
#include "WebApp.h"

StorageManager storage;
McpMatrix matrix;
TestEngine engine(matrix, storage);
WebApp web(storage, engine);

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("1024 Cable Assembly Tester / ESP32-S3 N16R8");

    String storageError;
    const bool storageReady = storage.begin(storageError);
    if (!storageReady) {
        Serial.println("Storage error: " + storageError);
    } else {
        Serial.println("LittleFS + FFat mounted");
    }

    String matrixError;
    const bool matrixReady = matrix.begin(matrixError);
    if (!matrixReady) {
        Serial.println("MCP matrix error: " + matrixError);
    } else {
        Serial.println("64 MCP23017 initialized");
    }

    const bool hardwareReady = storageReady && matrixReady;
    engine.begin(hardwareReady, storageReady ? matrixError : storageError);

    String networkError;
    if (!web.begin(networkError)) {
        Serial.println("Ethernet error: " + networkError);
    } else {
        Serial.println("Web interface: http://" + web.ipAddress() + "/");
    }
}

void loop() {
    web.loop();
    delay(1);
}
