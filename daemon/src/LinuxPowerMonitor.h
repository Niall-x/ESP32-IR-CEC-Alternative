#pragma once
#include "IPowerMonitor.h"

#include <sdbus-c++/sdbus-c++.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

struct udev;
struct udev_monitor;
struct udev_device;

class LinuxPowerMonitor : public IPowerMonitor {
public:
    LinuxPowerMonitor(uint16_t vid, uint16_t pid);
    ~LinuxPowerMonitor() override;

    void setOnCommand(std::function<bool(const TvCommand&)> cb) override;

    void setOnDeviceChange(std::function<void(bool present)> cb) override;

    void run() override;

private:
    bool takeInhibitorLock();

    void releaseInhibitorLock();

    void onPrepareForSleep(bool start);
    void onPrepareForShutdown(bool start);

    void assertTv(bool on, const char* reason);

    bool startDeviceWatch();
    void stopDeviceWatch();
    void drainDeviceEvents();
    bool isOurDevice(udev_device* dev) const;
    bool isOurRemoval(udev_device* dev) const;

    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<sdbus::IProxy>      proxy_;

    std::optional<sdbus::UnixFd> inhibitorFd_;

    std::function<bool(const TvCommand&)> onCommand_;
    std::function<void(bool)>             onDeviceChange_;

    uint16_t vid_;
    uint16_t pid_;

    udev*         udev_        = nullptr;
    udev_monitor* deviceWatch_ = nullptr;
    int           deviceWatchFd_ = -1;
};
