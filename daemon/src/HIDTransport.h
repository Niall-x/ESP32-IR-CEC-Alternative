#pragma once
#include "ITransport.h"
#include <hidapi/hidapi.h>
#include <chrono>
#include <cstdint>
#include <string>

class HIDTransport : public ITransport {
public:
    HIDTransport(uint16_t vid, uint16_t pid);
    ~HIDTransport() override;

    bool send(const std::string& cmd) override;

    void invalidate() override;

private:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    bool ensureOpen();

    bool reopenBefore(TimePoint deadline);

    hid_device* openMatching();

    void closeDevice();

    void drainInput();

    uint8_t nextSequence();

    bool awaitAck(uint8_t seq, const std::string& cmd, TimePoint deadline);

    uint16_t    vid_;
    uint16_t    pid_;
    hid_device* dev_ = nullptr;
    uint8_t     seq_ = 0;
};
