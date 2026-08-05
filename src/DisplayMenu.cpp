#include "DisplayMenu.h"

#include "AppConfig.h"
#include "AuxI2cBus.h"
#include <esp32-hal-log.h>

namespace {

constexpr uint8_t MENU_ITEM_COUNT = 7;
constexpr uint32_t DASHBOARD_REFRESH_MS = 500;
constexpr TickType_t MENU_TASK_PERIOD = pdMS_TO_TICKS(5);

} // namespace

DisplayMenu::DisplayMenu(NetworkSettings& settings, TestEngine& engine, WebApp& web)
    : settings_(settings),
      engine_(engine),
      web_(web),
      display_(AppConfig::OLED_WIDTH,
               AppConfig::OLED_HEIGHT,
               &Wire1,
               -1,
               AppConfig::RTC_I2C_CLOCK_HZ,
               AppConfig::RTC_I2C_CLOCK_HZ) {}

bool DisplayMenu::begin(String& error) {
    pinMode(AppConfig::ENCODER_A_PIN, INPUT_PULLUP);
    pinMode(AppConfig::ENCODER_B_PIN, INPUT_PULLUP);
    pinMode(AppConfig::ENCODER_BUTTON_PIN, INPUT_PULLUP);

    encoderPrevious_ = static_cast<uint8_t>((digitalRead(AppConfig::ENCODER_A_PIN) << 1) |
                                             digitalRead(AppConfig::ENCODER_B_PIN));
    buttonRaw_ = digitalRead(AppConfig::ENCODER_BUTTON_PIN) != LOW;
    buttonStable_ = buttonRaw_;
    buttonChangedAt_ = millis();

    AuxI2cBus::begin();
    if (AppConfig::OLED_I2C_SCAN_ON_BOOT) scanOledI2cBus();
    bool displayReady = false;
    if (AuxI2cBus::lock()) {
        displayReady = display_.begin(SSD1306_SWITCHCAPVCC, AppConfig::OLED_I2C_ADDRESS, true, false);
        AuxI2cBus::unlock();
    }
    if (!displayReady) {
        error = "GM009605/SSD1306 ne otvechaet na otdelnoy I2C-shine";
        return false;
    }

    if (!AppConfig::OLED_DIAGNOSTIC_DISABLE_UPDATES) {
        display_.clearDisplay();
        display_.setTextSize(1);
        display_.setTextColor(SSD1306_WHITE);
        display_.setTextWrap(false);
        display_.setCursor(0, 0);
        display_.println("KSK-1024 START");
        display_.println("ASYNC OLED MENU");
        if (AuxI2cBus::lock()) {
            display_.display();
            AuxI2cBus::unlock();
        }
    }

    working_ = settings_.snapshot();
    dirty_ = true;
    render();

    if (xTaskCreatePinnedToCore(taskThunk,
                                "oled-menu",
                                6144,
                                this,
                                1,
                                &taskHandle_,
                                1) != pdPASS) {
        taskHandle_ = nullptr;
        error = "Ne udalos sozdat FreeRTOS-zadachu OLED-menyu";
        return false;
    }
    return true;
}

void DisplayMenu::taskThunk(void* parameter) {
    static_cast<DisplayMenu*>(parameter)->taskLoop();
}

void DisplayMenu::scanOledI2cBus() {
    log_i("Wire1 scan started");
    bool foundAny = false;
    for (uint8_t address = AppConfig::OLED_I2C_SCAN_FIRST_ADDRESS;
         address <= AppConfig::OLED_I2C_SCAN_LAST_ADDRESS;
         ++address) {
        AuxI2cBus::wire().beginTransmission(address);
        const uint8_t result = AuxI2cBus::wire().endTransmission();
        if (result == 0) {
            foundAny = true;
            log_i("Wire1 device found at 0x%02X", address);
        }
        delay(1);
    }
    if (!foundAny) log_w("Wire1 scan: no devices found");
    log_i("Wire1 scan finished");
}

void DisplayMenu::taskLoop() {
    if (AppConfig::OLED_DIAGNOSTIC_DISABLE_UPDATES) {
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    for (;;) {
        const int8_t step = pollEncoder();
        const bool pressed = pollButtonPressed();
        if (step != 0 || pressed) handleInput(step, pressed);

        const uint32_t now = millis();
        if (rebootAt_ != 0 && static_cast<int32_t>(now - rebootAt_) >= 0) {
            display_.clearDisplay();
            display_.setTextColor(SSD1306_WHITE);
            display_.setCursor(0, 20);
            display_.println("RESTART...");
            if (AuxI2cBus::lock()) {
                display_.display();
                AuxI2cBus::unlock();
            }
            ESP.restart();
        }

        const bool dashboardRefresh = screen_ == Screen::Dashboard &&
                                      static_cast<uint32_t>(now - lastRenderAt_) >= DASHBOARD_REFRESH_MS;
        if (dirty_ || dashboardRefresh) render();
        vTaskDelay(MENU_TASK_PERIOD);
    }
}

int8_t DisplayMenu::pollEncoder() {
    static constexpr int8_t transitionTable[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0
    };

    const uint8_t current = static_cast<uint8_t>((digitalRead(AppConfig::ENCODER_A_PIN) << 1) |
                                                  digitalRead(AppConfig::ENCODER_B_PIN));
    const uint8_t transition = static_cast<uint8_t>((encoderPrevious_ << 2) | current);
    encoderPrevious_ = current;
    encoderAccumulator_ += transitionTable[transition & 0x0F];

    if (encoderAccumulator_ >= 4) {
        encoderAccumulator_ = 0;
        return 1;
    }
    if (encoderAccumulator_ <= -4) {
        encoderAccumulator_ = 0;
        return -1;
    }
    return 0;
}

bool DisplayMenu::pollButtonPressed() {
    const bool rawHigh = digitalRead(AppConfig::ENCODER_BUTTON_PIN) != LOW;
    const uint32_t now = millis();
    if (rawHigh != buttonRaw_) {
        buttonRaw_ = rawHigh;
        buttonChangedAt_ = now;
    }
    if (rawHigh != buttonStable_ &&
        static_cast<uint32_t>(now - buttonChangedAt_) >= AppConfig::ENCODER_BUTTON_DEBOUNCE_MS) {
        buttonStable_ = rawHigh;
        return !buttonStable_;
    }
    return false;
}

void DisplayMenu::handleInput(int8_t step, bool pressed) {
    switch (screen_) {
        case Screen::Dashboard:
            if (pressed) {
                working_ = settings_.snapshot();
                selectedItem_ = 0;
                screen_ = Screen::Menu;
                dirty_ = true;
            }
            break;

        case Screen::Menu:
            if (step != 0) {
                int value = static_cast<int>(selectedItem_) + step;
                if (value < 0) value = MENU_ITEM_COUNT - 1;
                if (value >= MENU_ITEM_COUNT) value = 0;
                selectedItem_ = static_cast<uint8_t>(value);
                dirty_ = true;
            }
            if (pressed) activateMenuItem();
            break;

        case Screen::EditAddress:
            if (step != 0) adjustAddressOctet(step);
            if (pressed) {
                ++editOctet_;
                if (editOctet_ >= 4) {
                    editOctet_ = 0;
                    screen_ = Screen::Menu;
                }
                dirty_ = true;
            }
            break;

        case Screen::Message:
            if (pressed && rebootAt_ == 0) {
                screen_ = Screen::Dashboard;
                dirty_ = true;
            }
            break;
    }
}

void DisplayMenu::activateMenuItem() {
    switch (selectedItem_) {
        case 0:
            working_.dhcp = !working_.dhcp;
            dirty_ = true;
            break;
        case 1:
            editField_ = AddressField::Ip;
            editOctet_ = 0;
            screen_ = Screen::EditAddress;
            dirty_ = true;
            break;
        case 2:
            editField_ = AddressField::Subnet;
            editOctet_ = 0;
            screen_ = Screen::EditAddress;
            dirty_ = true;
            break;
        case 3:
            editField_ = AddressField::Gateway;
            editOctet_ = 0;
            screen_ = Screen::EditAddress;
            dirty_ = true;
            break;
        case 4:
            editField_ = AddressField::Dns;
            editOctet_ = 0;
            screen_ = Screen::EditAddress;
            dirty_ = true;
            break;
        case 5: {
            String error;
            if (settings_.save(working_, error)) {
                message_ = "SAVED\nREBOOT IN 1 SEC";
                rebootAt_ = millis() + 1200;
            } else {
                message_ = "SAVE ERROR\n" + error;
                rebootAt_ = 0;
            }
            screen_ = Screen::Message;
            dirty_ = true;
            break;
        }
        case 6:
            working_ = settings_.snapshot();
            screen_ = Screen::Dashboard;
            dirty_ = true;
            break;
    }
}

void DisplayMenu::adjustAddressOctet(int8_t step) {
    IPAddress& address = editedAddress();
    int value = static_cast<int>(address[editOctet_]) + step;
    if (value < 0) value = 255;
    if (value > 255) value = 0;
    address[editOctet_] = static_cast<uint8_t>(value);
    dirty_ = true;
}

IPAddress& DisplayMenu::editedAddress() {
    switch (editField_) {
        case AddressField::Subnet: return working_.subnet;
        case AddressField::Gateway: return working_.gateway;
        case AddressField::Dns: return working_.dns;
        default: return working_.ip;
    }
}

void DisplayMenu::render() {
    display_.clearDisplay();
    display_.setTextSize(1);
    display_.setTextWrap(false);
    switch (screen_) {
        case Screen::Dashboard: renderDashboard(); break;
        case Screen::Menu: renderMenu(); break;
        case Screen::EditAddress: renderAddressEditor(); break;
        case Screen::Message: renderMessage(); break;
    }
    if (AuxI2cBus::lock()) {
        display_.display();
        AuxI2cBus::unlock();
    }
    lastRenderAt_ = millis();
    dirty_ = false;
}

void DisplayMenu::renderDashboard() {
    const EngineSnapshot snapshot = engine_.snapshot();
    const NetworkConfig config = settings_.snapshot();

    if (AppConfig::UI_ONLY_MODE) {
        printLine(0, "KSK-1024  UI ONLY");
        printLine(10, web_.currentClockText());
        printLine(20, String("LINK: ") + (web_.linkUp() ? "UP" : "DOWN"));
        printLine(30, "IP: " + web_.ipAddress());
        printLine(40, String("MODE: ") + (config.dhcp ? "DHCP" : "STATIC"));
        printLine(50, "MATRIX: DISABLED");
    } else if (snapshot.state == TestState::Scanning) {
        printLine(0, "TEST IN PROGRESS");
        printLine(10, web_.currentClockText());
        printLine(20, "MODE: " + modeLabel(snapshot.mode));
        printLine(30, String(snapshot.progressPercent) + "%  " +
                         String(snapshot.currentSource) + "/" + String(snapshot.totalSources));
        drawProgressBar(0, 42, AppConfig::OLED_WIDTH, 10, snapshot.progressPercent);
        printLine(54, "ELAP: " + String(snapshot.elapsedMs / 1000.0f, 1) + "S");
    } else if (snapshot.state == TestState::Preparing ||
               snapshot.state == TestState::Analyzing ||
               snapshot.state == TestState::Saving) {
        printLine(0, "TEST ACTIVE");
        printLine(10, web_.currentClockText());
        printLine(20, "MODE: " + modeLabel(snapshot.mode));
        printLine(30, "STATE: " + String(stateLabel(snapshot.state)));
        printLine(40, compactFileLabel(snapshot.activeReference));
        printLine(50, "PROGRESS: " + String(snapshot.progressPercent) + "%");
    } else if (snapshot.state == TestState::Completed || snapshot.state == TestState::Failed) {
        const bool passed = snapshot.state == TestState::Completed &&
                            snapshot.statistics.missingLinks == 0 &&
                            snapshot.statistics.extraLinks == 0 &&
                            snapshot.statistics.asymmetricLinks == 0 &&
                            snapshot.statistics.sourceDriveErrors == 0 &&
                            snapshot.statistics.i2cErrors == 0;
        printLine(0, snapshot.state == TestState::Failed ? "TEST ERROR" :
                  passed ? "RESULT: PASS" : "RESULT: FAIL");
        printLine(10, web_.currentClockText());
        if (snapshot.mode == TestMode::Compare) {
            printLine(20, "MODE: " + modeLabel(snapshot.mode));
            printLine(30, "MISS:" + String(snapshot.statistics.missingLinks) +
                             " EXT:" + String(snapshot.statistics.extraLinks));
            printLine(40, "ASYM:" + String(snapshot.statistics.asymmetricLinks));
            printLine(50, "DRV:" + String(snapshot.statistics.sourceDriveErrors) +
                             " I2C:" + String(snapshot.statistics.i2cErrors));
        } else {
            printLine(20, "MODE: " + modeLabel(snapshot.mode));
            printLine(30, "REFERENCE SAVED");
            printLine(40, "LOW:" + String(snapshot.statistics.stuckLowPins) +
                             " DRV:" + String(snapshot.statistics.sourceDriveErrors));
            printLine(50, "I2C:" + String(snapshot.statistics.i2cErrors));
        }
    } else {
        printLine(0, "KSK-1024  ASYNC");
        printLine(10, web_.currentClockText());
        printLine(20, String("LINK: ") + (web_.linkUp() ? "UP" : "DOWN"));
        printLine(30, "IP: " + web_.ipAddress());
        printLine(40, String("MODE: ") + (config.dhcp ? "DHCP" : "STATIC"));
        printLine(50, "PRESS: MENU");
    }
    display_.drawFastHLine(0, 62, AppConfig::OLED_WIDTH, SSD1306_WHITE);
}

void DisplayMenu::renderMenu() {
    static const char* labels[MENU_ITEM_COUNT] = {
        "DHCP", "IP", "MASK", "GATEWAY", "DNS", "SAVE+REBOOT", "CANCEL"
    };

    printLine(0, "MENU  PRESS=OK");
    const uint8_t first = selectedItem_ > 2 ? selectedItem_ - 2 : 0;
    for (uint8_t row = 0; row < 6; ++row) {
        const uint8_t item = first + row;
        if (item >= MENU_ITEM_COUNT) break;
        String value = labels[item];
        if (item == 0) value += working_.dhcp ? ": ON" : ": OFF";
        if (item == 1) value += ": " + working_.ip.toString();
        if (item == 2) value += ": " + working_.subnet.toString();
        if (item == 3) value += ": " + working_.gateway.toString();
        if (item == 4) value += ": " + working_.dns.toString();
        printLine(10 + row * 9, value, item == selectedItem_);
    }
}

void DisplayMenu::renderAddressEditor() {
    const IPAddress& address = editedAddress();
    printLine(0, "EDIT " + addressLabel(editField_));
    printLine(12, address.toString());

    String octets;
    for (uint8_t i = 0; i < 4; ++i) {
        if (i != 0) octets += "  ";
        octets += String(address[i]);
    }
    printLine(28, octets);
    printLine(42, "OCTET " + String(editOctet_ + 1) + " / 4");
    printLine(54, "PRESS: NEXT");
}

void DisplayMenu::renderMessage() {
    int16_t y = 12;
    int start = 0;
    while (start < static_cast<int>(message_.length()) && y < 64) {
        int end = message_.indexOf('\n', start);
        if (end < 0) end = message_.length();
        printLine(y, message_.substring(start, end));
        start = end + 1;
        y += 12;
    }
}

void DisplayMenu::printLine(int16_t y, const String& text, bool selected) {
    display_.setTextColor(selected ? SSD1306_BLACK : SSD1306_WHITE);
    if (selected) display_.fillRect(0, y, AppConfig::OLED_WIDTH, 9, SSD1306_WHITE);
    display_.setCursor(1, y + 1);
    display_.print(text.substring(0, 21));
    display_.setTextColor(SSD1306_WHITE);
}

void DisplayMenu::drawProgressBar(int16_t x,
                                  int16_t y,
                                  int16_t width,
                                  int16_t height,
                                  uint8_t percent) {
    const int16_t clampedWidth = width > 2 ? width - 2 : 0;
    const int16_t fillWidth = static_cast<int16_t>((static_cast<uint32_t>(clampedWidth) * percent) / 100U);
    display_.drawRect(x, y, width, height, SSD1306_WHITE);
    if (fillWidth > 0) {
        display_.fillRect(x + 1, y + 1, fillWidth, height - 2, SSD1306_WHITE);
    }
}

const char* DisplayMenu::stateLabel(TestState state) {
    switch (state) {
        case TestState::Preparing: return "PREPARE";
        case TestState::Scanning: return "SCAN";
        case TestState::Analyzing: return "ANALYZE";
        case TestState::Saving: return "SAVE";
        case TestState::Completed: return "DONE";
        case TestState::Failed: return "ERROR";
        default: return "IDLE";
    }
}

String DisplayMenu::addressLabel(AddressField field) {
    switch (field) {
        case AddressField::Subnet: return "MASK";
        case AddressField::Gateway: return "GATEWAY";
        case AddressField::Dns: return "DNS";
        default: return "IP";
    }
}

String DisplayMenu::modeLabel(TestMode mode) {
    switch (mode) {
        case TestMode::CaptureReference: return "REFERENCE";
        case TestMode::Compare: return "COMPARE";
        default: return "IDLE";
    }
}

String DisplayMenu::compactFileLabel(const char* value, size_t keep) {
    String text = value == nullptr ? "" : String(value);
    if (text.isEmpty()) return "NO ACTIVE FILE";
    if (text.length() <= keep) return text;
    return text.substring(0, keep);
}
