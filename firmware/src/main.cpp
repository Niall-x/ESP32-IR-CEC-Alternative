#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "USB.h"
#include "USBHID.h"
#include "esp_task_wdt.h"

#include "tusb.h"
#include "Profiles.h"
#include "Button.h"
#include "Display.h"

#define IR_TX_PIN    4
#define OLED_SDA     2
#define OLED_SCL     3
#define BUTTON_PIN   5

#define OLED_WIDTH  128
#define OLED_HEIGHT  32
#define OLED_ADDR   0x3C

#define DEVICE_VID  0x1209
#define DEVICE_PID  0x0001
#define REPORT_SIZE    64

#define WIFI_SSID  "ESP32-IR-Remote"
#define WIFI_PASS  "irremote123"

// Reboots the stick if loop() stops running. Sized by the web server, which can
// legitimately block for ~12s when a browser stalls mid-request.
#define WDT_TIMEOUT_SECONDS 20

static const uint8_t REPORT_DESCRIPTOR[] = {
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,
    0x09, 0x01,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, REPORT_SIZE,
    0x81, 0x02,
    0x09, 0x02,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, REPORT_SIZE,
    0x91, 0x02,
    0xC0
};

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
IRsend           irSend(IR_TX_PIN);
USBHID           HID;
Button           button(BUTTON_PIN);
Display          display(oled);
WebServer        server(80);

bool    wifiActive = false;

uint8_t pressCount = 0;

class VendorHID : public USBHIDDevice {
public:
    VendorHID() {}

    void begin() { HID.addDevice(this, sizeof(REPORT_DESCRIPTOR)); }

    uint16_t _onGetDescriptor(uint8_t* dst) override {
        memcpy(dst, REPORT_DESCRIPTOR, sizeof(REPORT_DESCRIPTOR));
        return sizeof(REPORT_DESCRIPTOR);
    }

    void _onOutput(uint8_t report_id, const uint8_t* buffer, uint16_t len) override {
        if (len < 2) return;

        uint16_t copyLen = min((int)len, REPORT_SIZE - 1);
        memcpy(rxBuf_, buffer, copyLen);
        rxBuf_[copyLen] = '\0';
        received_ = true;
    }

    bool send(const uint8_t* data) {
        return HID.SendReport(0, data, REPORT_SIZE);
    }

    volatile bool received_     = false;
    uint8_t rxBuf_[REPORT_SIZE] = {0};
};

VendorHID hidDevice;

volatile bool usbResumePending = false;
volatile bool usbWasSuspended  = false;

static void onUsbEvent(void*, esp_event_base_t,
                       int32_t id, void*) {
    switch (id) {
    case ARDUINO_USB_SUSPEND_EVENT:
        usbWasSuspended = true;
        break;
    case ARDUINO_USB_RESUME_EVENT:
        if (usbWasSuspended) {
            usbWasSuspended  = false;
            usbResumePending = true;
        }
        break;
    default:
        break;
    }
}

static void serviceUsbRecovery() {
    if (!usbResumePending) return;
    usbResumePending = false;

    Serial.println("[usb] Resumed from suspend — re-enumerating");

    tud_disconnect();
    delay(100);
    tud_connect();
}

bool sendIR(const Profile& profile, bool on) {
    if (!Profiles::isConfigured(profile, on)) {
        Serial.printf("[ir] %s has no %s code configured — not transmitting\n",
                      profile.name.c_str(), on ? "ON" : "OFF");
        return false;
    }

    uint32_t code = on ? profile.onCode : profile.offCode;
    switch (profile.protocol) {
        case IrProtocol::SAMSUNG:
            irSend.sendSAMSUNG(code, kSamsungBits);
            break;
        case IrProtocol::SONY:
            // 12 bits, not 20. SIRC has three lengths, and a TV is category 1,
            // which is the 12-bit form — at 20 bits the same command becomes a
            // different waveform and the TV does not answer.
            irSend.sendSony(code, kSony12Bits);
            break;
        case IrProtocol::NEC:
        default:
            irSend.sendNEC(code, kNECBits);
            break;
    }
    return true;
}

void stopWifi();

void startWifi() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    String apIP = WiFi.softAPIP().toString();
    Serial.printf("[wifi] AP started — SSID: %s  IP: %s\n", WIFI_SSID, apIP.c_str());
    display.setWifiIP(apIP);

    server.on("/api/profiles", HTTP_GET, []() {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();

        for (const auto& p : Profiles::getAll()) {
            Profiles::toJson(p, arr.add<JsonObject>());
        }
        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json);
    });

    server.on("/api/profiles", HTTP_POST, []() {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, server.arg("plain"));
        if (err) {
            server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }

        std::vector<Profile> newProfiles;
        for (JsonObjectConst obj : doc.as<JsonArrayConst>()) {
            newProfiles.push_back(Profiles::fromJson(obj));
        }
        if (newProfiles.empty()) {
            server.send(400, "application/json", "{\"error\":\"At least one profile required\"}");
            return;
        }
        if (newProfiles.size() > Profiles::MAX_PROFILES) {
            server.send(400, "application/json",
                        "{\"error\":\"Too many profiles\"}");
            return;
        }
        Profiles::replaceAll(std::move(newProfiles));
        Profiles::saveProfiles();
        Profiles::saveSettings();
        display.showStatus(Profiles::getActive().name, true);
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/settings", HTTP_GET, []() {
        const Settings& s = Profiles::getSettings();
        JsonDocument doc;
        doc["active_profile"]    = s.activeProfile;
        doc["display_always_on"] = s.displayAlwaysOn;
        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json);
    });

    server.on("/api/settings", HTTP_POST, []() {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, server.arg("plain"));
        if (err) {
            server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }

        Settings& s = Profiles::getMutableSettings();
        if (doc["active_profile"].is<int>()) {
            int idx = doc["active_profile"].as<int>();
            if (idx >= 0 && idx < (int)Profiles::getAll().size()) {
                s.activeProfile = idx;
            }
        }
        if (doc["display_always_on"].is<bool>()) {
            s.displayAlwaysOn = doc["display_always_on"].as<bool>();
        }
        Profiles::saveSettings();
        display.showStatus(Profiles::getActive().name, true);
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/factory-reset", HTTP_POST, []() {
        Profiles::factoryReset();
        server.send(200, "application/json", "{\"ok\":true}");
        wifiActive = false;
    });

    server.on("/api/exit", HTTP_POST, []() {
        server.send(200, "application/json", "{\"ok\":true}");

        wifiActive = false;
    });

    server.onNotFound([]() {
        String path = server.uri();
        if (path == "/") path = "/index.html";

        if (LittleFS.exists(path)) {
            String contentType = "text/plain";
            if      (path.endsWith(".html")) contentType = "text/html";
            else if (path.endsWith(".css"))  contentType = "text/css";
            else if (path.endsWith(".js"))   contentType = "application/javascript";
            else if (path.endsWith(".json")) contentType = "application/json";

            File f = LittleFS.open(path, "r");
            server.streamFile(f, contentType);
            f.close();
        } else {
            server.send(404, "text/plain", "Not Found");
        }
    });

    server.begin();
    Serial.println("[wifi] Web server started on port 80");
}

void stopWifi() {
    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[wifi] AP stopped");
}

void finishExitWifi() {
    if (WiFi.getMode() != WIFI_OFF) stopWifi();
    wifiActive = false;
    display.setWifiActive(false);
    display.showStatus(Profiles::getActive().name,
                       Profiles::getSettings().displayAlwaysOn);
    pressCount = 1;
}

void onButtonPress() {
    pressCount++;
    bool alwaysOn = Profiles::getSettings().displayAlwaysOn || wifiActive;

    if (wifiActive) {
        if (pressCount == 1) {
            display.showStatus(Profiles::getActive().name, true);
        } else {
            display.showWifiLockMessage();
            pressCount = 1;
        }
        return;
    }

    if (pressCount == 1) {
        display.showStatus(Profiles::getActive().name, alwaysOn);
    } else {
        int next = Profiles::nextVisibleIndex();
        Profiles::getMutableSettings().activeProfile = next;
        Profiles::saveSettings();

        display.showStatus(Profiles::getActive().name, alwaysOn);
        pressCount = 1;
    }
}

void onButtonHold(uint32_t heldMs) {
    pressCount = 0;

    if (heldMs >= HoldTimings::RESET_END_MS) {
        // The reset has fired and put the status screen up; leave it there.
        return;
    } else if (heldMs >= HoldTimings::RESET_START_MS) {
        display.showResetBar(heldMs);
    } else if (heldMs >= HoldTimings::CONFIG_MS) {
        display.showConfigRelease(!wifiActive);
    } else {
        display.showHoldBar(heldMs, !wifiActive);
    }
}

void onConfigThreshold() {
    if (wifiActive) {
        finishExitWifi();
    } else {
        wifiActive = true;
        display.setWifiActive(true);
        startWifi();

        display.showStatus(Profiles::getActive().name, true);
        pressCount = 1;
    }
}

void onFactoryReset() {
    Serial.println("[app] Factory reset triggered");
    Profiles::factoryReset();
    finishExitWifi();
}

void setup() {
    Serial.begin(115200);

    if (!Profiles::begin()) {
        Serial.println("[app] Profile storage unavailable — using fallback profile");
    }

    Wire.begin(OLED_SDA, OLED_SCL);
    if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        display.begin();
    }

    button.begin();
    button.onPress           = onButtonPress;
    button.onHold            = onButtonHold;
    button.onHoldCancelled   = []() {
        bool alwaysOn = Profiles::getSettings().displayAlwaysOn || wifiActive;
        display.showStatus(Profiles::getActive().name, alwaysOn);
        pressCount = 1;
    };
    button.onConfigThreshold = onConfigThreshold;
    button.onFactoryReset    = onFactoryReset;

    display.onExpire = []() { pressCount = 0; };

    display.showStatus(Profiles::getActive().name,
                       Profiles::getSettings().displayAlwaysOn);

    irSend.begin();

    hidDevice.begin();
    USB.VID(DEVICE_VID);
    USB.PID(DEVICE_PID);
    USB.productName("ESP32 IR Remote");
    USB.manufacturerName("ESP32-IR-CEC");

    USB.onEvent(ARDUINO_USB_SUSPEND_EVENT, onUsbEvent);
    USB.onEvent(ARDUINO_USB_RESUME_EVENT, onUsbEvent);

    USB.begin();
    HID.begin();

    // Armed last, so first-boot flash formatting never has to fit the timeout.
    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
    esp_task_wdt_add(NULL);
    Serial.printf("[wdt] Watchdog armed (%ds)\n", WDT_TIMEOUT_SECONDS);
}

void loop() {
    esp_task_wdt_reset();

    button.update();
    display.update();

    serviceUsbRecovery();

    if (wifiActive) {
        server.handleClient();
    } else if (WiFi.getMode() != WIFI_OFF) {
        // A web request asked to leave config mode. The AP is torn down here
        // rather than in the handler, so the HTTP reply gets out first.
        finishExitWifi();
    }

    if (!hidDevice.received_) return;
    hidDevice.received_ = false;

    const uint8_t seq = hidDevice.rxBuf_[0];

    String cmd = String((char*)hidDevice.rxBuf_ + 1);
    cmd.trim();

    const bool alwaysOn = Profiles::getSettings().displayAlwaysOn || wifiActive;
    const Profile& active = Profiles::getActive();

    uint8_t response[REPORT_SIZE] = {0};
    response[0] = seq;

    bool ok = false;
    if (cmd == "ON" || cmd == "OFF") {
        const bool on = (cmd == "ON");
        ok = sendIR(active, on);
        if (ok) {
            display.showIRConfirm(on, alwaysOn, active.name);
        } else {
            display.showNotConfigured(alwaysOn, active.name);
        }
    }

    memcpy(response + 1, ok ? "ACK" : "ERR", 3);

    if (!hidDevice.send(response)) {
        Serial.println("[hid] Failed to queue response report");
    }
}
