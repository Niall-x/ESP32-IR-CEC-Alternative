#pragma once

#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <functional>

#include "HoldTimings.h"

class Display {
public:
    std::function<void()> onExpire;

    Display(Adafruit_SSD1306& oled);

    void begin();
    void update();

    void showStatus(const String& profileName, bool alwaysOn);

    void showIRConfirm(bool on, bool alwaysOn, const String& profileName);

    void showHoldBar(uint32_t heldMs, bool enteringWifi);

    void showResetBar(uint32_t heldMs);

    void showConfigRelease(bool entering);

    void showWifiLockMessage();

    void showNotConfigured(bool alwaysOn, const String& profileName);

    void off();

    void setWifiActive(bool active);

    void setWifiIP(const String& ip);

private:
    Adafruit_SSD1306& oled_;
    bool              ok_         = false;
    bool              wifiActive_ = false;
    String            wifiIP_;

    bool     timerActive_   = false;
    uint32_t timerStart_    = 0;
    uint32_t timerDuration_ = 0;

    enum class TimerAction { TurnOff, ShowStatus };
    TimerAction timerAction_ = TimerAction::TurnOff;

    String lastProfile_;

    enum class BarKind : uint8_t { None, Hold, Reset };
    BarKind barKind_    = BarKind::None;
    int16_t barFilled_  = -1;
    bool    barFlag_    = false;

    void resetBarCache();
    void drawStatus(const String& profileName);
    void drawProgressBar(uint8_t filled, uint8_t total, uint8_t y);

    static constexpr uint32_t STATUS_TIMEOUT_MS     = 2000;
    static constexpr uint32_t IR_CONFIRM_TIMEOUT_MS = 2000;
    static constexpr uint32_t WIFI_MSG_TIMEOUT_MS   = 2000;
};
