#ifdef _WIN32

#include "WindowsPowerMonitor.h"

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <iostream>
#include <string>

#include <dbt.h>

static constexpr wchar_t SERVICE_NAME[] = L"esp32-ir-remote";

static const GUID CONSOLE_DISPLAY_STATE = {
    0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 }
};

static const GUID HID_DEVICE_INTERFACE = {
    0x4d1e55b2, 0xf16f, 0x11cf, { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 }
};

static constexpr DWORD DISPLAY_OFF    = 0;
static constexpr DWORD DISPLAY_ON     = 1;
static constexpr DWORD DISPLAY_DIMMED = 2;

static WindowsPowerMonitor* g_instance = nullptr;

static VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    if (g_instance) g_instance->serviceMain(argc, argv);
}

static DWORD WINAPI ServiceCtrlHandlerEx(DWORD control, DWORD eventType,
                                         LPVOID eventData, LPVOID) {
    if (!g_instance) return ERROR_CALL_NOT_IMPLEMENTED;
    return g_instance->serviceCtrlHandler(control, eventType, eventData);
}

static void plog(const std::string& msg) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u",
                  t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::cout << ("[power] " + std::string(stamp) + " " + msg + "\n");
}

static std::string formatSeconds(long long totalSeconds) {
    if (totalSeconds < 0) return "unknown";
    if (totalSeconds < 90) return std::to_string(totalSeconds) + "s";

    const long long minutes = totalSeconds / 60;
    const long long seconds = totalSeconds % 60;
    if (minutes < 90) {
        return std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
    }
    const long long hours = minutes / 60;
    return std::to_string(hours) + "h " + std::to_string(minutes % 60) + "m";
}

WindowsPowerMonitor::WindowsPowerMonitor(uint16_t vid, uint16_t pid)
    : vid_(vid), pid_(pid) {
    g_instance = this;
}

WindowsPowerMonitor::~WindowsPowerMonitor() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopRequested_ = true;
    }
    queueCv_.notify_all();
    if (worker_.joinable()) worker_.join();

    if (displayNotify_) {
        UnregisterPowerSettingNotification(displayNotify_);
        displayNotify_ = nullptr;
    }
    if (deviceNotify_) {
        UnregisterDeviceNotification(deviceNotify_);
        deviceNotify_ = nullptr;
    }
    if (workerDone_) {
        CloseHandle(workerDone_);
        workerDone_ = nullptr;
    }
    g_instance = nullptr;
}

void WindowsPowerMonitor::setOnCommand(std::function<bool(const TvCommand&)> cb) {
    onCommand_ = std::move(cb);
}

void WindowsPowerMonitor::setOnDeviceChange(std::function<void(bool)> cb) {
    onDeviceChange_ = std::move(cb);
}

const char* WindowsPowerMonitor::reasonFor(Trigger why, bool on) {
    switch (why) {
    case Trigger::Suspend:       return "sleep";
    case Trigger::Resume:        return "wake";
    case Trigger::DeviceArrival: return "ESP32 reconnected";
    case Trigger::DisplayState:
    default:                     return on ? "display on" : "display off";
    }
}

void WindowsPowerMonitor::run() {
    SERVICE_TABLE_ENTRYW dispatchTable[] = {
        { const_cast<LPWSTR>(SERVICE_NAME), ServiceMain },
        { nullptr, nullptr }
    };

    if (StartServiceCtrlDispatcherW(dispatchTable)) return;

    const DWORD err = GetLastError();
    if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        std::cerr << "[monitor] Not started by the Service Control Manager.\n"
                     "[monitor] Power events are only delivered to a service. Install with\n"
                     "[monitor] packaging/windows/install-service.ps1, or pass --console to\n"
                     "[monitor] check the ESP32 is reachable without installing anything.\n";
    } else {
        std::cerr << "[error] StartServiceCtrlDispatcher failed: " << err << "\n";
    }
}

void WindowsPowerMonitor::serviceMain(DWORD, LPWSTR*) {
    serviceStartedAt_ = std::chrono::steady_clock::now();

    statusHandle_ = RegisterServiceCtrlHandlerExW(SERVICE_NAME, ServiceCtrlHandlerEx, nullptr);
    if (!statusHandle_) {
        std::cerr << "[error] RegisterServiceCtrlHandlerEx failed: " << GetLastError() << "\n";
        return;
    }

    status_.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    status_.dwServiceSpecificExitCode = 0;
    status_.dwControlsAccepted        = 0;
    reportStatus(SERVICE_START_PENDING);

    workerDone_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!workerDone_) {
        std::cerr << "[error] CreateEvent failed: " << GetLastError() << "\n";
        reportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.dwControlsAccepted = SERVICE_ACCEPT_STOP
                                   | SERVICE_ACCEPT_PRESHUTDOWN
                                   | SERVICE_ACCEPT_POWEREVENT;
    }
    reportStatus(SERVICE_RUNNING);

    // The console display state is the primary signal: the TV mirrors whatever
    // the PC's own screen is doing. Suspend and resume are kept as a second
    // trigger underneath it, not as a separate policy.
    displayNotify_ = RegisterPowerSettingNotification(
        reinterpret_cast<HANDLE>(statusHandle_), &CONSOLE_DISPLAY_STATE,
        DEVICE_NOTIFY_SERVICE_HANDLE);

    if (!displayNotify_) {
        std::cerr << "[monitor] RegisterPowerSettingNotification failed: " << GetLastError()
                  << " — falling back to suspend/resume events only. An idle screen\n"
                     "[monitor] blank and the ON at boot will not be seen in this mode.\n";
    }

    {
        DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
        filter.dbcc_size       = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid  = HID_DEVICE_INTERFACE;

        deviceNotify_ = RegisterDeviceNotificationW(
            reinterpret_cast<HANDLE>(statusHandle_), &filter,
            DEVICE_NOTIFY_SERVICE_HANDLE);

        if (!deviceNotify_) {
            std::cerr << "[monitor] RegisterDeviceNotification failed: " << GetLastError()
                      << " — device arrival and removal will not be seen. The transport\n"
                         "[monitor] still recovers by reopening on a failed write.\n";
        }
    }

    worker_ = std::thread(&WindowsPowerMonitor::workerLoop, this);

    std::cout << "[monitor] Windows Service running\n";

    WaitForSingleObject(workerDone_, INFINITE);
    if (worker_.joinable()) worker_.join();

    if (displayNotify_) {
        UnregisterPowerSettingNotification(displayNotify_);
        displayNotify_ = nullptr;
    }
    if (deviceNotify_) {
        UnregisterDeviceNotification(deviceNotify_);
        deviceNotify_ = nullptr;
    }
    reportStatus(SERVICE_STOPPED);
}

bool WindowsPowerMonitor::deviceNameMatches(const wchar_t* name) const {
    if (!name) return false;

    wchar_t needle[32];
    std::swprintf(needle, sizeof(needle) / sizeof(needle[0]),
                  L"VID_%04X&PID_%04X",
                  static_cast<unsigned>(vid_), static_cast<unsigned>(pid_));

    std::wstring haystack(name);
    for (wchar_t& c : haystack) c = static_cast<wchar_t>(std::towupper(c));

    return haystack.find(needle) != std::wstring::npos;
}

void WindowsPowerMonitor::handleDeviceEvent(DWORD eventType, LPVOID eventData) {
    if (eventType != DBT_DEVICEARRIVAL && eventType != DBT_DEVICEREMOVECOMPLETE) {
        return;
    }

    const auto* hdr = static_cast<const DEV_BROADCAST_HDR*>(eventData);
    if (!hdr || hdr->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE) return;

    const auto* iface = reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(hdr);
    if (!deviceNameMatches(iface->dbcc_name)) return;

    if (eventType == DBT_DEVICEARRIVAL) {
        plog("ESP32 arrived");

        // Arrival re-asserts only while the service is still starting up, where
        // it is what delivers the boot ON the stick was not present to receive.
        // Later on it would override a TV somebody turned off by hand.
        const auto sinceStart = std::chrono::steady_clock::now() - serviceStartedAt_;

        if (sinceStart > ARRIVAL_GRACE) {
            plog("past the arrival grace window — leaving the TV alone");
        } else if (desiredOn_.has_value()) {
            requestState(*desiredOn_, Trigger::DeviceArrival);
        } else {
            plog("no state decided yet — nothing to re-assert");
        }
    } else {
        plog("ESP32 removed");

        requestDeviceInvalidate();
    }
}

void WindowsPowerMonitor::logAwayTime() {
    if (!suspendWallAt_.has_value()) return;

    const auto away = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now() - *suspendWallAt_);
    plog("away for " + formatSeconds(away.count()));
    suspendWallAt_.reset();
}

DWORD WindowsPowerMonitor::serviceCtrlHandler(DWORD control, DWORD eventType, LPVOID eventData) {
    switch (control) {
    case SERVICE_CONTROL_INTERROGATE: {
        std::lock_guard<std::mutex> lock(statusMutex_);
        SetServiceStatus(statusHandle_, &status_);
        return NO_ERROR;
    }

    case SERVICE_CONTROL_PRESHUTDOWN:
        plog("SERVICE_CONTROL_PRESHUTDOWN");
        reportStatus(SERVICE_STOP_PENDING);
        requestShutdown();
        return NO_ERROR;

    case SERVICE_CONTROL_STOP:
        std::cout << "[monitor] Service stop requested\n";
        reportStatus(SERVICE_STOP_PENDING);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopRequested_ = true;
        }
        queueCv_.notify_all();
        return NO_ERROR;

    case SERVICE_CONTROL_DEVICEEVENT:
        handleDeviceEvent(eventType, eventData);
        return NO_ERROR;

    case SERVICE_CONTROL_POWEREVENT:
        switch (eventType) {
        case PBT_APMSUSPEND:
            plog("PBT_APMSUSPEND (sleep or hibernate)");
            suspendWallAt_ = std::chrono::system_clock::now();
            requestState(false, Trigger::Suspend);
            return NO_ERROR;

        case PBT_APMRESUMESUSPEND:
            logAwayTime();
            if (lastDisplayState_.has_value() && !*lastDisplayState_) {
                plog("PBT_APMRESUMESUSPEND while display is known off — not asserting on");
                return NO_ERROR;
            }
            plog("PBT_APMRESUMESUSPEND (user-initiated resume)");
            requestState(true, Trigger::Resume);
            return NO_ERROR;

        case PBT_APMRESUMEAUTOMATIC:
            logAwayTime();
            plog("PBT_APMRESUMEAUTOMATIC (resume, presence unknown) — not acted on");
            return NO_ERROR;

        case PBT_APMRESUMECRITICAL:
            logAwayTime();
            plog("PBT_APMRESUMECRITICAL (resume after unannounced power loss) — not acted on");
            return NO_ERROR;

        case PBT_POWERSETTINGCHANGE: {
            const auto* setting = static_cast<const POWERBROADCAST_SETTING*>(eventData);
            if (!setting || !IsEqualGUID(setting->PowerSetting, CONSOLE_DISPLAY_STATE)) {
                return NO_ERROR;
            }
            if (setting->DataLength < sizeof(DWORD)) {
                plog("display-state notification with unexpected payload — ignored");
                return NO_ERROR;
            }

            DWORD state = 0;
            std::memcpy(&state, setting->Data, sizeof(state));

            switch (state) {
            case DISPLAY_OFF:
                // Unconditional. Windows has already blanked every display,
                // the TV among them, so there is no screen left to black out
                // by acting on this — only one left showing black.
                plog("display state = off");
                lastDisplayState_ = false;
                requestState(false, Trigger::DisplayState);
                break;
            case DISPLAY_ON:
                plog("display state = on");
                logAwayTime();
                lastDisplayState_ = true;
                requestState(true, Trigger::DisplayState);
                break;
            case DISPLAY_DIMMED:
                plog("display state = dimmed (treated as on)");
                lastDisplayState_ = true;
                requestState(true, Trigger::DisplayState);
                break;
            default:
                plog("display state = " + std::to_string(state) + " (unknown) — ignored");
                break;
            }
            return NO_ERROR;
        }

        default:
            plog("unhandled power event type " + std::to_string(eventType));
            return NO_ERROR;
        }

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WindowsPowerMonitor::requestState(bool on, Trigger why) {
    desiredOn_ = on;

    {
        std::lock_guard<std::mutex> lock(queueMutex_);

        pendingOn_  = on;
        pendingWhy_ = why;

        if (why == Trigger::Suspend) {
            suspendAnnouncedAt_ = std::chrono::steady_clock::now();
        }
    }
    queueCv_.notify_all();
}

void WindowsPowerMonitor::requestShutdown() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        shutdownRequested_ = true;
    }
    queueCv_.notify_all();
}

void WindowsPowerMonitor::requestDeviceInvalidate() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        deviceGone_ = true;
    }
    queueCv_.notify_all();
}

void WindowsPowerMonitor::workerLoop() {
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCv_.wait_for(lock, INITIAL_STATE_WAIT, [this] {
            return pendingOn_.has_value() || shutdownRequested_ || stopRequested_ || deviceGone_;
        });

        if (!pendingOn_.has_value()) {
            plog("no display state reported at registration — opening state "
                 "UNKNOWN, asserting nothing");
            plog("  the TV will be driven from the first display-state change");
            plog("  on a normal boot Windows reports the state immediately, so "
                 "this line means the ON at boot did not happen");
        }
    }

    while (true) {
        std::optional<bool> want;
        Trigger  why          = Trigger::DisplayState;
        bool     doShutdown   = false;
        bool     doStop       = false;
        bool     doInvalidate = false;
        std::optional<std::chrono::steady_clock::time_point> suspendAt;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] {
                return pendingOn_.has_value() || shutdownRequested_
                    || stopRequested_ || deviceGone_;
            });
            want = pendingOn_;
            pendingOn_.reset();
            why          = pendingWhy_;
            doShutdown   = shutdownRequested_;
            doStop       = stopRequested_;
            doInvalidate = deviceGone_;
            deviceGone_  = false;
            suspendAt    = suspendAnnouncedAt_;
        }

        if (doInvalidate) {
            if (onDeviceChange_) {
                try { onDeviceChange_(false); } catch (...) {}
            }
        }

        if (doShutdown) {
            std::cout << "[event] Shutting down\n";
            reportPending();
            if (onCommand_) {
                try {
                    (void)onCommand_(TvCommand{false, "shutdown"});
                } catch (...) {}
            }
            reportPending();
            break;
        }

        if (want.has_value()) {
            const bool on = *want;

            switch (why) {
            case Trigger::DisplayState:
                std::cout << (on ? "[event] Display on\n" : "[event] Display off\n");
                break;
            case Trigger::Suspend:
                std::cout << "[event] Going to sleep\n";
                break;
            case Trigger::Resume:
                std::cout << "[event] Woke up\n";
                break;
            case Trigger::DeviceArrival:
                std::cout << (on ? "[event] ESP32 reconnected — re-asserting on\n"
                                 : "[event] ESP32 reconnected — re-asserting off\n");
                break;
            }
            reportPending();

            if (onCommand_) {
                try {
                    (void)onCommand_(TvCommand{on, reasonFor(why, on)});
                } catch (...) {
                }
            }

            if (why == Trigger::Suspend && suspendAt.has_value()) {
                const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - *suspendAt);
                plog("suspend send took " + std::to_string(used.count()) + "ms");
            }
        }

        if (doStop) break;
    }

    plog("worker finished");
    if (workerDone_) SetEvent(workerDone_);
}

void WindowsPowerMonitor::reportStatus(DWORD state, DWORD exitCode) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.dwCurrentState  = state;
    status_.dwWin32ExitCode = exitCode;

    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        status_.dwWaitHint = PENDING_WAIT_HINT_MS;
        status_.dwCheckPoint++;
    } else {
        status_.dwWaitHint   = 0;
        status_.dwCheckPoint = 0;
    }
    SetServiceStatus(statusHandle_, &status_);
}

void WindowsPowerMonitor::reportPending() {
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (status_.dwCurrentState != SERVICE_STOP_PENDING &&
        status_.dwCurrentState != SERVICE_START_PENDING) {
        return;
    }
    status_.dwCheckPoint++;
    SetServiceStatus(statusHandle_, &status_);
}

#endif
