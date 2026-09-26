#include "Button.h"

Button::Button(uint8_t pin, bool pullup)
    : pin_(pin), pullup_(pullup) {}

void Button::begin() {
    pinMode(pin_, pullup_ ? INPUT_PULLUP : INPUT);
}

void Button::update() {
    bool raw = pullup_ ? (digitalRead(pin_) == LOW) : (digitalRead(pin_) == HIGH);

    if (raw != lastRaw_) {
        lastDebounce_ = millis();
        lastRaw_ = raw;
    }

    if ((millis() - lastDebounce_) < HoldTimings::DEBOUNCE_MS) return;

    if (raw && !pressed_) {
        pressed_    = true;
        resetFired_ = false;
        pressTime_  = millis();
        return;
    }

    if (!raw && pressed_) {
        pressed_        = false;
        uint32_t held   = millis() - pressTime_;

        if (held < HoldTimings::PRESS_MAX_MS) {
            if (onPress) onPress();
        } else if (held < HoldTimings::CONFIG_MS) {
            if (onHoldCancelled) onHoldCancelled();
        } else if (!resetFired_) {
            if (held < HoldTimings::RESET_START_MS) {
                if (onConfigThreshold) onConfigThreshold();
            } else {
                if (onHoldCancelled) onHoldCancelled();
            }
        }

        return;
    }

    if (pressed_) {
        uint32_t held = millis() - pressTime_;

        if (held >= HoldTimings::PRESS_MAX_MS) {
            if (onHold) onHold(held);

            if (held >= HoldTimings::RESET_END_MS && !resetFired_) {
                resetFired_ = true;
                if (onFactoryReset) onFactoryReset();
            }
        }
    }
}
