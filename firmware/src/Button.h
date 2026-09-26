#pragma once

#include <Arduino.h>
#include <functional>

#include "HoldTimings.h"

class Button {
public:
    std::function<void()> onPress;
    std::function<void(uint32_t)> onHold;

    std::function<void()> onHoldCancelled;
    std::function<void()> onConfigThreshold;
    std::function<void()> onFactoryReset;

    Button(uint8_t pin, bool pullup = true);

    void begin();

    void update();

private:
    uint8_t  pin_;
    bool     pullup_;
    bool     lastRaw_       = false;
    bool     pressed_       = false;
    bool     resetFired_    = false;
    uint32_t pressTime_     = 0;
    uint32_t lastDebounce_  = 0;
};
