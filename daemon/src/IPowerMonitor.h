#pragma once
#include <functional>

struct TvCommand {
    bool on;
    const char* reason;
};

class IPowerMonitor {
public:
    virtual void setOnCommand(std::function<bool(const TvCommand&)> cb) = 0;

    virtual void setOnDeviceChange(std::function<void(bool present)>) {}

    virtual void run() = 0;

    virtual ~IPowerMonitor() = default;
};
