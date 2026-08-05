#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "NetworkSettings.h"
#include "TestEngine.h"
#include "WebApp.h"

class DisplayMenu {
public:
    DisplayMenu(NetworkSettings& settings, TestEngine& engine, WebApp& web);
    bool begin(String& error);

private:
    enum class Screen : uint8_t { Dashboard, Menu, EditAddress, Message };
    enum class AddressField : uint8_t { Ip, Subnet, Gateway, Dns };

    NetworkSettings& settings_;
    TestEngine& engine_;
    WebApp& web_;
    Adafruit_SSD1306 display_;
    TaskHandle_t taskHandle_ = nullptr;

    Screen screen_ = Screen::Dashboard;
    AddressField editField_ = AddressField::Ip;
    NetworkConfig working_;
    uint8_t selectedItem_ = 0;
    uint8_t editOctet_ = 0;
    int8_t encoderAccumulator_ = 0;
    uint8_t encoderPrevious_ = 0;
    bool buttonRaw_ = true;
    bool buttonStable_ = true;
    uint32_t buttonChangedAt_ = 0;
    uint32_t lastRenderAt_ = 0;
    uint32_t rebootAt_ = 0;
    bool dirty_ = true;
    String message_;

    static void taskThunk(void* parameter);
    void taskLoop();
    int8_t pollEncoder();
    bool pollButtonPressed();
    void handleInput(int8_t step, bool pressed);
    void activateMenuItem();
    void adjustAddressOctet(int8_t step);
    IPAddress& editedAddress();

    void render();
    void renderDashboard();
    void renderMenu();
    void renderAddressEditor();
    void renderMessage();
    void printLine(int16_t y, const String& text, bool selected = false);
    void drawProgressBar(int16_t x, int16_t y, int16_t width, int16_t height, uint8_t percent);
    void scanOledI2cBus();

    static const char* stateLabel(TestState state);
    static String addressLabel(AddressField field);
    static String modeLabel(TestMode mode);
    static String compactFileLabel(const char* value, size_t keep = 14);
};
