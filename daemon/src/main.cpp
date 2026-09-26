#include "IPowerMonitor.h"
#include "ITransport.h"
#include "HIDTransport.h"

#ifdef __linux__
#include "LinuxPowerMonitor.h"
#elif defined(_WIN32)
#include "WindowsPowerMonitor.h"
#include <windows.h>
#include <cstdio>
#include <string>
#endif

#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

static constexpr uint16_t DEVICE_VID = 0x1209;
static constexpr uint16_t DEVICE_PID = 0x0001;

#ifdef __linux__

static constexpr double BOOT_WINDOW_SECONDS = 180.0;

// Guards the boot ON against a package upgrade restarting the service at 3am.
static bool systemJustBooted() {
    std::ifstream uptimeFile("/proc/uptime");
    double uptimeSeconds = 0.0;
    if (uptimeFile >> uptimeSeconds) {
        return uptimeSeconds < BOOT_WINDOW_SECONDS;
    }

    return true;
}

#endif

#ifdef _WIN32

static bool consoleMode() {
    const wchar_t* cmdline = GetCommandLineW();
    return cmdline && wcsstr(cmdline, L"--console") != nullptr;
}

static HANDLE g_singleInstance = nullptr;

static bool claimSingleInstance() {
    g_singleInstance = CreateMutexW(nullptr, TRUE, L"Global\\ESP32IRRemote_SingleInstance");
    const DWORD err = GetLastError();

    if (g_singleInstance && err != ERROR_ALREADY_EXISTS) return true;

    if (g_singleInstance) {
        CloseHandle(g_singleInstance);
        g_singleInstance = nullptr;
    }
    return false;
}

static std::wstring logDirectory() {
    wchar_t env[MAX_PATH] = {0};
    const DWORD len = GetEnvironmentVariableW(L"ProgramData", env, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::wstring(env) + L"\\ESP32IRRemote";
    }
    return L"C:\\ProgramData\\ESP32IRRemote";
}

static void redirectLogsToFile() {
    const std::wstring dir  = logDirectory();
    CreateDirectoryW(dir.c_str(), nullptr);

    const std::wstring path = dir + L"\\daemon.log";
    const std::wstring prev = dir + L"\\daemon.log.old";

    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        const ULONGLONG size =
            (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (size > 1024ULL * 1024ULL) {
            MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    FILE* out = nullptr;
    FILE* err = nullptr;
    _wfreopen_s(&out, path.c_str(), L"a", stdout);
    _wfreopen_s(&err, path.c_str(), L"a", stderr);
}

#endif

int main() {
    std::cout << std::unitbuf;

#ifdef _WIN32

    const bool console = consoleMode();
    if (console) {
        SetConsoleOutputCP(CP_UTF8);
    } else {
        redirectLogsToFile();
    }

    if (!claimSingleInstance()) {
        std::cerr << "[error] Another instance is already running — refusing to start.\n"
                     "[error] Stop the esp32-ir-remote service before running by hand;\n"
                     "[error] two openers of the same HID device steal each other's ACKs.\n";
        return 1;
    }
#endif

    std::unique_ptr<ITransport> transport;
    try {
        transport = std::make_unique<HIDTransport>(DEVICE_VID, DEVICE_PID);
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 1;
    }

    std::unique_ptr<IPowerMonitor> monitor;

#ifdef __linux__
    try {
        monitor = std::make_unique<LinuxPowerMonitor>(DEVICE_VID, DEVICE_PID);
        std::cout << "[monitor] Connected to systemd-logind\n";
    } catch (const std::exception& e) {
        std::cerr << "[error] Failed to connect to D-Bus: " << e.what() << "\n";
        return 1;
    }
#elif defined(_WIN32)
    monitor = std::make_unique<WindowsPowerMonitor>(DEVICE_VID, DEVICE_PID);
    std::cout << "[monitor] Windows Service power monitor initialised\n";
#else
    std::cerr << "[error] No power monitor available for this platform\n";
    return 1;
#endif

    auto report = [](const char* cmd, const char* reason, bool ok) {
        if (ok) {
            std::cout << "[cmd] " << cmd << " sent and ACK received (" << reason << ")\n";
        } else {
            std::cerr << "[cmd] " << cmd << " FAILED — no ACK, TV state not changed ("
                      << reason << ")\n";
        }
        return ok;
    };

    monitor->setOnCommand([&](const TvCommand& c) {
        const char* cmd = c.on ? "ON" : "OFF";
        return report(cmd, c.reason, transport->send(cmd));
    });

#ifdef __linux__
    bool bootOnPending = false;
#endif

    monitor->setOnDeviceChange([&](bool present) {
        if (!present) {
            transport->invalidate();
            return;
        }
#ifdef __linux__
        // Delivers the boot ON when the stick enumerates after the daemon starts.
        if (bootOnPending && systemJustBooted()) {
            bootOnPending = !report("ON", "device ready", transport->send("ON"));
        }
#endif
    });

#ifdef __linux__

    if (systemJustBooted()) {
        bootOnPending = !report("ON", "startup", transport->send("ON"));
        if (bootOnPending) {
            std::cout << "[cmd] ON deferred until the ESP32 appears\n";
        }
    } else {
        std::cout << "[cmd] ON skipped — service restarted on an already-running "
                     "system, not a boot\n";
    }
#endif

#ifdef _WIN32

    if (console) {
        std::cout << "[sys] Console mode — checking the ESP32 responds, then exiting\n";
        const bool ok = report("ON", "console check", transport->send("ON"));
        return ok ? 0 : 1;
    }
#endif

    std::cout << "[sys] Daemon running\n";
    monitor->run();

    return 0;
}
