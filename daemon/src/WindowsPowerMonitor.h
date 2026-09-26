#pragma once
#ifdef _WIN32

#include "IPowerMonitor.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <windows.h>

class WindowsPowerMonitor : public IPowerMonitor {
public:
    WindowsPowerMonitor(uint16_t vid, uint16_t pid);
    ~WindowsPowerMonitor() override;

    void setOnCommand(std::function<bool(const TvCommand&)> cb) override;
    void setOnDeviceChange(std::function<void(bool)> cb) override;

    void run() override;

    void serviceMain(DWORD argc, LPWSTR* argv);
    DWORD serviceCtrlHandler(DWORD control, DWORD eventType, LPVOID eventData);

private:
    static constexpr auto INITIAL_STATE_WAIT = std::chrono::milliseconds(2000);

    // How long after service start a device arrival still counts as the stick
    // arriving at boot, rather than a replug. Measured from service start, not
    // uptime, because Fast Startup makes uptime meaningless.
    static constexpr auto ARRIVAL_GRACE = std::chrono::seconds(180);

    // What Windows is told to expect while a start or stop is in progress.
    static constexpr DWORD PENDING_WAIT_HINT_MS = 30000;

    enum class Trigger { DisplayState, Suspend, Resume, DeviceArrival };

    void workerLoop();
    void requestState(bool on, Trigger why);
    void requestShutdown();
    void requestDeviceInvalidate();

    static const char*               reasonFor(Trigger why, bool on);

    bool deviceNameMatches(const wchar_t* name) const;

    void handleDeviceEvent(DWORD eventType, LPVOID eventData);

    void logAwayTime();

    void reportStatus(DWORD state, DWORD exitCode = NO_ERROR);
    void reportPending();

    uint16_t vid_;
    uint16_t pid_;

    std::function<bool(const TvCommand&)> onCommand_;
    std::function<void(bool)>             onDeviceChange_;

    SERVICE_STATUS        status_       = {};
    SERVICE_STATUS_HANDLE statusHandle_ = nullptr;
    std::mutex            statusMutex_;

    HPOWERNOTIFY displayNotify_ = nullptr;
    HDEVNOTIFY   deviceNotify_  = nullptr;

    HANDLE workerDone_ = nullptr;

    std::thread             worker_;
    std::mutex              queueMutex_;
    std::condition_variable queueCv_;

    std::optional<bool> pendingOn_;
    Trigger             pendingWhy_ = Trigger::DisplayState;
    bool                shutdownRequested_ = false;
    bool                stopRequested_     = false;

    bool                deviceGone_ = false;

    std::optional<std::chrono::steady_clock::time_point> suspendAnnouncedAt_;

    std::optional<std::chrono::system_clock::time_point> suspendWallAt_;

    std::optional<bool> desiredOn_;

    std::optional<bool> lastDisplayState_;

    // Written and read on the SCM's thread only, so no lock.
    std::chrono::steady_clock::time_point serviceStartedAt_{};
};

#endif
