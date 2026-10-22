#include "Display.h"

Display::Display(Adafruit_SSD1306& oled) : oled_(oled) {}

void Display::begin() {
    ok_ = true;
    off();
}

void Display::update() {
    if (!ok_ || !timerActive_) return;

    if ((millis() - timerStart_) >= timerDuration_) {
        timerActive_ = false;
        switch (timerAction_) {
            case TimerAction::TurnOff:
                off();

                if (onExpire) onExpire();
                break;
            case TimerAction::ShowStatus:
                drawStatus(lastProfile_);
                break;
        }
    }
}

void Display::showStatus(const String& profileName, bool alwaysOn) {
    if (!ok_) return;
    lastProfile_ = profileName;

    drawStatus(profileName);

    if (!alwaysOn) {
        timerActive_   = true;
        timerStart_    = millis();
        timerDuration_ = STATUS_TIMEOUT_MS;
        timerAction_   = TimerAction::TurnOff;
    } else {
        timerActive_ = false;
    }
}

void Display::showIRConfirm(bool on, bool alwaysOn, const String& profileName) {
    if (!ok_) return;
    lastProfile_ = profileName;
    resetBarCache();

    oled_.clearDisplay();
    oled_.setTextSize(2);
    oled_.setTextColor(SSD1306_WHITE);
    oled_.setCursor(0, 8);
    oled_.print(on ? "TV On" : "TV Off");
    oled_.display();

    timerActive_   = true;
    timerStart_    = millis();
    timerDuration_ = IR_CONFIRM_TIMEOUT_MS;
    timerAction_   = alwaysOn ? TimerAction::ShowStatus : TimerAction::TurnOff;
}

void Display::showHoldBar(uint32_t heldMs, bool enteringWifi) {
    if (!ok_) return;
    timerActive_ = false;

    // One block per second, derived from the threshold so the two stay in step.
    constexpr uint8_t BLOCKS = HoldTimings::CONFIG_MS / 1000;

    uint8_t filled = (uint8_t)(heldMs / 1000);
    if (filled > BLOCKS) filled = BLOCKS;

    if (barKind_ == BarKind::Hold && barFilled_ == filled && barFlag_ == enteringWifi) return;
    barKind_   = BarKind::Hold;
    barFilled_ = filled;
    barFlag_   = enteringWifi;

    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);

    oled_.setCursor(0, 0);
    oled_.print(enteringWifi ? "Enter Wireless Config?" : "Exit Wireless Config?");

    drawProgressBar(filled, BLOCKS, 20);

    oled_.display();
}

void Display::showResetBar(uint32_t heldMs) {
    if (!ok_) return;
    timerActive_ = false;

    // Also one block per second, counting from where this bar takes over.
    constexpr uint32_t BAR_START = HoldTimings::RESET_START_MS;
    constexpr uint8_t  BLOCKS =
        (HoldTimings::RESET_END_MS - BAR_START) / 1000;

    uint32_t elapsed = heldMs > BAR_START ? heldMs - BAR_START : 0;
    uint8_t  filled  = (uint8_t)(elapsed / 1000);
    if (filled > BLOCKS) filled = BLOCKS;

    if (barKind_ == BarKind::Reset && barFilled_ == filled) return;
    barKind_   = BarKind::Reset;
    barFilled_ = filled;

    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);

    oled_.setCursor(0, 0);
    oled_.print("Hold To Factory Reset");

    drawProgressBar(filled, BLOCKS, 20);

    oled_.display();
}

void Display::showConfigRelease(bool entering) {
    if (!ok_) return;
    timerActive_ = false;
    resetBarCache();

    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);
    oled_.setCursor(0, 0);
    oled_.print(entering ? "Release To Enter" : "Release To Exit");
    oled_.setCursor(0, 12);
    oled_.print("Wireless Config!");
    oled_.display();
}

void Display::showWifiLockMessage() {
    if (!ok_) return;
    resetBarCache();

    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);
    oled_.setCursor(0, 0);
    oled_.print("Hold Button to Exit");
    oled_.setCursor(0, 11);
    oled_.print("Wireless Mode to");
    oled_.setCursor(0, 22);
    oled_.print("Switch Profiles");
    oled_.display();

    timerActive_   = true;
    timerStart_    = millis();
    timerDuration_ = WIFI_MSG_TIMEOUT_MS;
    timerAction_   = TimerAction::ShowStatus;
}

void Display::showNotConfigured(bool alwaysOn, const String& profileName) {
    if (!ok_) return;
    lastProfile_ = profileName;
    resetBarCache();

    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);
    oled_.setCursor(0, 4);
    oled_.print("Not Configured");
    oled_.setCursor(0, 18);
    oled_.print(profileName);
    oled_.print(": no IR code");
    oled_.display();

    timerActive_   = true;
    timerStart_    = millis();
    timerDuration_ = IR_CONFIRM_TIMEOUT_MS;
    timerAction_   = alwaysOn ? TimerAction::ShowStatus : TimerAction::TurnOff;
}

void Display::off() {
    if (!ok_) return;
    timerActive_ = false;
    resetBarCache();
    oled_.clearDisplay();
    oled_.display();
}

void Display::setWifiActive(bool active) {
    wifiActive_ = active;
}

void Display::setWifiInfo(const String& ssid, const String& password,
                          const String& ip) {
    wifiSsid_     = ssid;
    wifiPassword_ = password;
    wifiIP_       = ip;
}

void Display::resetBarCache() {
    barKind_   = BarKind::None;
    barFilled_ = -1;
}

void Display::drawStatus(const String& profileName) {
    if (!ok_) return;
    resetBarCache();
    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);

    // Everything needed to connect, one line each: network, password, address.
    if (wifiActive_) {
        oled_.setCursor(0, 0);
        oled_.print("Profile: ");
        oled_.print(profileName);

        oled_.setCursor(0, 8);
        oled_.print(wifiSsid_);

        oled_.setCursor(0, 16);
        oled_.print("Pass: ");
        oled_.print(wifiPassword_);

        oled_.setCursor(0, 24);
        oled_.print("http://");
        oled_.print(wifiIP_);
    } else {
        oled_.setCursor(0, 12);
        oled_.print("Profile: ");
        oled_.print(profileName);
    }

    oled_.display();
}

void Display::drawProgressBar(uint8_t filled, uint8_t total, uint8_t y) {
    uint8_t blockW = (128 - (total - 1)) / total;
    uint8_t barH   = 10;

    for (uint8_t i = 0; i < total; i++) {
        uint8_t x = i * (blockW + 1);
        if (i < filled) {
            oled_.fillRect(x, y, blockW, barH, SSD1306_WHITE);
        } else {
            oled_.drawRect(x, y, blockW, barH, SSD1306_WHITE);
        }
    }
}
