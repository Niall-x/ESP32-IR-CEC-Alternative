#include "HIDTransport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

static constexpr size_t REPORT_SIZE = 64;

// One timeout for a whole send: opening, any retried write, and the ACK. A
// healthy round trip is under 200ms, and no OS deadline is shorter than this.
static constexpr auto SEND_TIMEOUT = std::chrono::milliseconds(2000);

static constexpr auto OPEN_POLL = std::chrono::milliseconds(100);

static constexpr int MAX_DRAIN_REPORTS = 64;

static constexpr int MAX_WRITE_ATTEMPTS = 4;

static constexpr auto WRITE_RETRY_DELAY = std::chrono::milliseconds(150);

static constexpr wchar_t EXPECTED_PRODUCT[] = L"ESP32 IR Remote";

static std::string narrow(const wchar_t* w) {
    if (!w) return "(none)";
    std::string out;
    for (const wchar_t* p = w; *p; ++p) {
        const auto c = static_cast<unsigned long>(*p);
        out += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
    }
    return out;
}

HIDTransport::HIDTransport(uint16_t vid, uint16_t pid) : vid_(vid), pid_(pid) {
    if (hid_init() != 0) {
        throw std::runtime_error("hid_init() failed — no usable HID backend");
    }

    if (!ensureOpen()) {
        std::cout << "[transport] ESP32 not found at startup — will retry when needed\n";
    }
}

HIDTransport::~HIDTransport() {
    closeDevice();
    hid_exit();
}

void HIDTransport::closeDevice() {
    if (dev_) {
        hid_close(dev_);
        dev_ = nullptr;
    }
}

void HIDTransport::invalidate() {
    if (!dev_) {
        std::cout << "[transport] Device removal reported; no handle was open\n";
        return;
    }
    std::cout << "[transport] Device removed — handle dropped, will reopen on next send\n";
    closeDevice();
}

hid_device* HIDTransport::openMatching() {
    hid_device_info* list = hid_enumerate(vid_, pid_);
    if (!list) return nullptr;

    int count = 0;
    for (const hid_device_info* d = list; d; d = d->next) ++count;

    if (count == 1) {
        hid_free_enumeration(list);
        return hid_open(vid_, pid_, nullptr);
    }

    std::cerr << "[transport] " << count << " devices match VID="
              << std::hex << vid_ << " PID=" << pid_ << std::dec
              << " — this is a shared test PID, not a unique identity."
                 " Candidates:\n";
    for (const hid_device_info* d = list; d; d = d->next) {
        std::cerr << "[transport]   product=\"" << narrow(d->product_string)
                  << "\" manufacturer=\"" << narrow(d->manufacturer_string)
                  << "\" interface=" << d->interface_number
                  << " usage_page=" << std::hex << d->usage_page
                  << " usage=" << d->usage << std::dec
                  << " path=" << (d->path ? d->path : "(none)") << "\n";
    }

    const hid_device_info* chosen = nullptr;
    for (const hid_device_info* d = list; d; d = d->next) {
        if (d->product_string && std::wcscmp(d->product_string, EXPECTED_PRODUCT) == 0) {
            chosen = d;
            break;
        }
    }

    hid_device* opened = nullptr;
    if (chosen) {
        std::cerr << "[transport] Selecting the one identifying as \""
                  << narrow(EXPECTED_PRODUCT) << "\"\n";
        opened = hid_open_path(chosen->path);
    } else {
        std::cerr << "[transport] None identifies as \"" << narrow(EXPECTED_PRODUCT)
                  << "\" — falling back to the first match, which may not be"
                     " the ESP32\n";
        opened = hid_open(vid_, pid_, nullptr);
    }

    hid_free_enumeration(list);
    return opened;
}

// One attempt, no waiting: with the stick unplugged, waiting would only delay
// sleep and shutdown.
bool HIDTransport::ensureOpen() {
    if (dev_) return true;

    dev_ = openMatching();
    if (!dev_) return false;

    std::cout << "[transport] HID device opened (VID="
              << std::hex << vid_ << " PID=" << pid_ << std::dec << ")\n";
    return true;
}

// Only called after an open handle stopped working, so the device exists and is
// probably re-enumerating. Worth waiting for.
bool HIDTransport::reopenBefore(TimePoint deadline) {
    while (!ensureOpen()) {
        if (Clock::now() + OPEN_POLL >= deadline) return false;
        std::this_thread::sleep_for(OPEN_POLL);
    }
    return true;
}

void HIDTransport::drainInput() {
    uint8_t scratch[REPORT_SIZE];
    for (int i = 0; i < MAX_DRAIN_REPORTS; ++i) {
        if (hid_read_timeout(dev_, scratch, sizeof(scratch), 0) <= 0) return;
    }
    std::cerr << "[transport] Input queue still not empty after draining "
              << MAX_DRAIN_REPORTS << " reports\n";
}

uint8_t HIDTransport::nextSequence() {
    if (++seq_ == 0) seq_ = 1;
    return seq_;
}

bool HIDTransport::send(const std::string& cmd) {
    const TimePoint deadline = Clock::now() + SEND_TIMEOUT;

    if (!ensureOpen()) {
        std::cerr << "[transport] ESP32 not connected — skipping IR command: " << cmd << "\n";
        return false;
    }

    drainInput();

    const uint8_t seq = nextSequence();

    uint8_t txBuf[REPORT_SIZE + 1] = {0};
    txBuf[0] = 0x00;
    txBuf[1] = seq;
    std::memcpy(txBuf + 2, cmd.c_str(), std::min(cmd.size(), REPORT_SIZE - 2));

    bool written = false;
    for (int attempt = 1; attempt <= MAX_WRITE_ATTEMPTS; ++attempt) {
        if (hid_write(dev_, txBuf, sizeof(txBuf)) >= 0) {
            written = true;
            if (attempt > 1) {
                std::cout << "[transport] Write succeeded on attempt " << attempt << "\n";
            }
            break;
        }

        std::cerr << "[transport] Write failed on attempt " << attempt << " ("
                  << narrow(hid_error(dev_)) << ")\n";

        closeDevice();
        if (Clock::now() + WRITE_RETRY_DELAY >= deadline) break;
        std::this_thread::sleep_for(WRITE_RETRY_DELAY);
        if (!reopenBefore(deadline)) break;
    }

    if (!written) {
        std::cerr << "[transport] Giving up on command: " << cmd << "\n";
        closeDevice();
        return false;
    }

    return awaitAck(seq, cmd, deadline);
}

bool HIDTransport::awaitAck(uint8_t seq, const std::string& cmd, TimePoint deadline) {
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());
        if (remaining.count() <= 0) break;

        uint8_t rxBuf[REPORT_SIZE] = {0};
        const int res = hid_read_timeout(dev_, rxBuf, sizeof(rxBuf),
                                         static_cast<int>(remaining.count()));

        if (res < 0) {
            std::cerr << "[transport] Read failed (" << narrow(hid_error(dev_))
                      << ") — dropping device handle\n";
            closeDevice();
            return false;
        }
        if (res == 0) break;

        if (res < 4 || rxBuf[0] != seq) {
            std::cerr << "[transport] Discarding unmatched reply (seq "
                      << static_cast<int>(rxBuf[0]) << ", expected "
                      << static_cast<int>(seq) << ") — stale reply, or firmware "
                         "too old for the sequenced protocol\n";
            continue;
        }

        const char* body = reinterpret_cast<const char*>(rxBuf) + 1;

        if (std::strncmp(body, "ACK", 3) == 0) {
            return true;
        }
        if (std::strncmp(body, "ERR", 3) == 0) {
            std::cerr << "[transport] ESP32 returned ERR for command: " << cmd
                      << " — command unknown, or the active profile has no code"
                         " configured for this direction\n";
            return false;
        }

        std::cerr << "[transport] Unexpected response for command: " << cmd << "\n";
        return false;
    }

    std::cerr << "[transport] No response received for command: " << cmd << "\n";
    return false;
}
