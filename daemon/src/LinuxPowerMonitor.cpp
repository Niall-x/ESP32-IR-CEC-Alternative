#include "LinuxPowerMonitor.h"

#include <fcntl.h>
#include <libudev.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

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

    if (udev_monitor_filter_add_match_subsystem_devtype(deviceWatch_, "hidraw", nullptr) < 0) {
        std::cerr << "[monitor] Could not filter the udev monitor to hidraw\n";
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

// The stick's USB side freezes across a sleep unless the PC resets it on wake.
// Some ports do that as part of resuming and some resume the stick as it was,
// which leaves it accepting nothing until it is replugged. Verified on hardware:
// a frozen stick timed out every write, then ACKed straight after one reset.
// Resetting on every wake makes all ports behave like the ones that work.
void LinuxPowerMonitor::resetDevice() {
    udev* ctx = udev_new();
    if (!ctx) {
        std::cerr << "[monitor] udev_new() failed — ESP32 not reset\n";
        return;
    }

    char vendor[8];
    char product[8];
    std::snprintf(vendor, sizeof(vendor), "%04x", vid_);
    std::snprintf(product, sizeof(product), "%04x", pid_);

    udev_enumerate* found = udev_enumerate_new(ctx);
    udev_enumerate_add_match_subsystem(found, "usb");
    udev_enumerate_add_match_property(found, "DEVTYPE", "usb_device");
    udev_enumerate_add_match_sysattr(found, "idVendor", vendor);
    udev_enumerate_add_match_sysattr(found, "idProduct", product);
    udev_enumerate_scan_devices(found);

    udev_list_entry* first = udev_enumerate_get_list_entry(found);
    udev_device* dev = first
        ? udev_device_new_from_syspath(ctx, udev_list_entry_get_name(first))
        : nullptr;
    const char* node = dev ? udev_device_get_devnode(dev) : nullptr;

    if (!node) {
        std::cout << "[monitor] ESP32 not present — nothing to reset\n";
    } else {
        const int fd = ::open(node, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            std::cerr << "[monitor] Could not open " << node << " to reset the ESP32 ("
                      << std::strerror(errno) << "). Check the udev rule gives the"
                         " esp32ir group the usb_device node, and the unit's"
                         " DeviceAllow includes char-usb_device.\n";
        } else {
            if (::ioctl(fd, USBDEVFS_RESET, 0) < 0) {
                std::cerr << "[monitor] USB reset of the ESP32 failed ("
                          << std::strerror(errno) << ")\n";
            } else {
                std::cout << "[monitor] ESP32 reset after wake\n";
            }
            ::close(fd);
        }
    }

    if (dev) udev_device_unref(dev);
    udev_enumerate_unref(found);
    udev_unref(ctx);
}

void LinuxPowerMonitor::drainDeviceEvents() {
    while (udev_device* dev = udev_monitor_receive_device(deviceWatch_)) {
        const char* action = udev_device_get_action(dev);

        if (action && std::strcmp(action, "add") == 0 && isOurDevice(dev)) {
            std::cout << "[monitor] ESP32 appeared\n";
            if (onDeviceChange_) {
                try { onDeviceChange_(true); } catch (...) {}
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
        resetDevice();
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
