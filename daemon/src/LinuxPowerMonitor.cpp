#include "LinuxPowerMonitor.h"

#include <libudev.h>
#include <poll.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

static constexpr auto LOGIND_SERVICE   = "org.freedesktop.login1";
static constexpr auto LOGIND_OBJECT    = "/org/freedesktop/login1";
static constexpr auto LOGIND_INTERFACE = "org.freedesktop.login1.Manager";

LinuxPowerMonitor::LinuxPowerMonitor(uint16_t vid, uint16_t pid)
    : vid_(vid), pid_(pid) {
    connection_ = sdbus::createSystemBusConnection();

    proxy_ = sdbus::createProxy(*connection_,
        sdbus::ServiceName{LOGIND_SERVICE},
        sdbus::ObjectPath{LOGIND_OBJECT});

    proxy_->uponSignal("PrepareForSleep")
           .onInterface(LOGIND_INTERFACE)
           .call([this](bool start) { onPrepareForSleep(start); });

    proxy_->uponSignal("PrepareForShutdown")
           .onInterface(LOGIND_INTERFACE)
           .call([this](bool start) { onPrepareForShutdown(start); });

    if (!takeInhibitorLock()) {
        throw std::runtime_error("logind refused the initial inhibitor lock");
    }

    if (!startDeviceWatch()) {
        std::cerr << "[monitor] udev watch unavailable — a stick that enumerates"
                     " after this point will not be noticed\n";
    }
}

LinuxPowerMonitor::~LinuxPowerMonitor() {
    stopDeviceWatch();
    releaseInhibitorLock();
}

void LinuxPowerMonitor::setOnCommand(std::function<bool(const TvCommand&)> cb) {
    onCommand_ = std::move(cb);
}

void LinuxPowerMonitor::setOnDeviceChange(std::function<void(bool)> cb) {
    onDeviceChange_ = std::move(cb);
}

void LinuxPowerMonitor::assertTv(bool on, const char* reason) {
    if (!onCommand_) return;
    try {
        (void)onCommand_(TvCommand{on, reason});
    } catch (...) {
        // A failed send must never stop the machine sleeping or shutting down.
    }
}

// Each step reports its own failure. The netlink socket is the one that fails in
// practice, because of the unit's sandbox, so its message says where to look.
bool LinuxPowerMonitor::startDeviceWatch() {
    udev_ = udev_new();
    if (!udev_) {
        std::cerr << "[monitor] udev_new() failed\n";
        return false;
    }

    deviceWatch_ = udev_monitor_new_from_netlink(udev_, "udev");
    if (!deviceWatch_) {
        std::cerr << "[monitor] Could not open the udev netlink socket ("
                  << std::strerror(errno) << "). If the daemon is running as a"
                     " systemd unit, check RestrictAddressFamilies allows"
                     " AF_NETLINK.\n";
        stopDeviceWatch();
        return false;
    }

    if (udev_monitor_filter_add_match_subsystem_devtype(deviceWatch_, "hidraw", nullptr) < 0 ||
        udev_monitor_filter_add_match_subsystem_devtype(deviceWatch_, "usb", "usb_device") < 0) {
        std::cerr << "[monitor] Could not filter the udev monitor to hidraw and usb\n";
        stopDeviceWatch();
        return false;
    }

    if (udev_monitor_enable_receiving(deviceWatch_) < 0) {
        std::cerr << "[monitor] Could not enable the udev monitor\n";
        stopDeviceWatch();
        return false;
    }

    deviceWatchFd_ = udev_monitor_get_fd(deviceWatch_);
    if (deviceWatchFd_ < 0) {
        std::cerr << "[monitor] udev monitor returned no usable descriptor\n";
        stopDeviceWatch();
        return false;
    }

    std::cout << "[monitor] Watching udev for the ESP32\n";
    return true;
}

void LinuxPowerMonitor::stopDeviceWatch() {
    if (deviceWatch_) {
        udev_monitor_unref(deviceWatch_);
        deviceWatch_ = nullptr;
    }
    if (udev_) {
        udev_unref(udev_);
        udev_ = nullptr;
    }
    deviceWatchFd_ = -1;
}

// A hidraw node carries no USB ids of its own — they belong to the usb_device
// it hangs off. The parent is owned by dev and must not be unreferenced.
bool LinuxPowerMonitor::isOurDevice(udev_device* dev) const {
    udev_device* usb =
        udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
    if (!usb) return false;

    const char* vendor  = udev_device_get_sysattr_value(usb, "idVendor");
    const char* product = udev_device_get_sysattr_value(usb, "idProduct");
    if (!vendor || !product) return false;

    char expected[8];
    std::snprintf(expected, sizeof(expected), "%04x", vid_);
    if (std::strcmp(vendor, expected) != 0) return false;

    std::snprintf(expected, sizeof(expected), "%04x", pid_);
    return std::strcmp(product, expected) == 0;
}

// A removal cannot be matched like an arrival: by the time it is reported the
// sysfs entries isOurDevice() reads are gone. The usb_device's own event still
// carries PRODUCT, as "vid/pid/bcdDevice" in unpadded lowercase hex.
bool LinuxPowerMonitor::isOurRemoval(udev_device* dev) const {
    const char* product = udev_device_get_property_value(dev, "PRODUCT");
    if (!product) return false;

    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "%x/%x/", vid_, pid_);
    return std::strncmp(product, prefix, std::strlen(prefix)) == 0;
}

// Arrivals are taken from hidraw, so the node the transport opens exists by
// the time they are reported. Removals are taken from the usb_device.
void LinuxPowerMonitor::drainDeviceEvents() {
    while (udev_device* dev = udev_monitor_receive_device(deviceWatch_)) {
        const char* action    = udev_device_get_action(dev);
        const char* subsystem = udev_device_get_subsystem(dev);

        if (action && subsystem) {
            const bool hidraw = std::strcmp(subsystem, "hidraw") == 0;

            if (hidraw && std::strcmp(action, "add") == 0 && isOurDevice(dev)) {
                std::cout << "[monitor] ESP32 appeared\n";
                if (onDeviceChange_) {
                    try { onDeviceChange_(true); } catch (...) {}
                }
            } else if (!hidraw && std::strcmp(action, "remove") == 0 && isOurRemoval(dev)) {
                std::cout << "[monitor] ESP32 removed\n";
                if (onDeviceChange_) {
                    try { onDeviceChange_(false); } catch (...) {}
                }
            }
        }

        udev_device_unref(dev);
    }
}

void LinuxPowerMonitor::run() {
    if (deviceWatchFd_ < 0) {
        connection_->enterEventLoop();
        return;
    }

    while (true) {
        // sd-bus recalculates these after every batch of work, so they cannot
        // be hoisted out of the loop.
        const auto pd = connection_->getEventLoopPollData();

        pollfd fds[3] = {
            { pd.fd,          pd.events, 0 },
            { pd.eventFd,     POLLIN,    0 },
            { deviceWatchFd_, POLLIN,    0 },
        };

        if (::poll(fds, 3, pd.getPollTimeout()) < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[monitor] poll failed: " << std::strerror(errno) << "\n";
            return;
        }

        if (fds[2].revents & POLLIN) drainDeviceEvents();

        while (connection_->processPendingEvent()) {}
    }
}

bool LinuxPowerMonitor::takeInhibitorLock() {
    try {
        sdbus::UnixFd fd;
        proxy_->callMethod("Inhibit")
            .onInterface(LOGIND_INTERFACE)
            .withArguments(
                std::string("sleep:shutdown"),
                std::string("esp32-ir-remote"),
                std::string("Sending IR command to TV"),
                std::string("delay"))
            .storeResultsTo(fd);

        inhibitorFd_ = std::move(fd);
        std::cout << "[inhibitor] Lock acquired\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[inhibitor] Failed to acquire lock: " << e.what()
                  << " — sleep and shutdown will not be delayed\n";
        return false;
    }
}

void LinuxPowerMonitor::releaseInhibitorLock() {
    if (inhibitorFd_.has_value()) {
        inhibitorFd_.reset();
        std::cout << "[inhibitor] Lock released\n";
    }
}

void LinuxPowerMonitor::onPrepareForSleep(bool start) {
    if (start) {
        std::cout << "[event] Going to sleep\n";

        assertTv(false, "sleep");
        releaseInhibitorLock();
    } else {
        std::cout << "[event] Woke up\n";
        assertTv(true, "wake");
        takeInhibitorLock();
    }
}

void LinuxPowerMonitor::onPrepareForShutdown(bool start) {
    if (start) {
        std::cout << "[event] Shutting down\n";

        assertTv(false, "shutdown");
        releaseInhibitorLock();
    }

}
