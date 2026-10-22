#include "Profiles.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "bootloader_random.h"
#include "esp_random.h"

struct DefaultProfile {
    const char* name;
    const char* protocol;
    uint32_t    onCode;
    uint32_t    offCode;
    bool        visible;
};

// Discrete power codes. Where they came from, and why TCL and Hisense are
// absent, is in the brief under "Where IR codes come from".
static const DefaultProfile DEFAULT_PROFILES[] = {
    { "LG",      "NEC",     0x20DF23DC, 0x20DFA35C, true },
    { "Samsung", "SAMSUNG", 0xE0E09966, 0xE0E019E6, true },
    { "Sony",    "SONY",    0x00000750, 0x00000F50, true },
    { "Toshiba", "NEC",     0x02FD7E81, 0x02FDFE01, true },
};

static const Settings DEFAULT_SETTINGS = {
    .activeProfile   = 0,
    .displayAlwaysOn = false,
    .wifiPassword    = "",
};

static constexpr size_t WIFI_PASSWORD_LENGTH = 8;

// Read off a 128x32 screen, so no characters that look alike (0/O, 1/l/I).
// 8 of 31 symbols is about 40 bits, plenty for a network that is only up
// while somebody is standing at the stick.
static String generateWifiPassword() {
    static const char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";

    // The RNG is only truly random while the radio or this entropy source is
    // on. The source must not be switched on under a running radio, which a
    // factory reset from the setup page does; the radio is enough then.
    const bool radioOff = WiFi.getMode() == WIFI_OFF;
    if (radioOff) bootloader_random_enable();

    String pass;
    for (size_t i = 0; i < WIFI_PASSWORD_LENGTH; i++) {
        pass += ALPHABET[esp_random() % (sizeof(ALPHABET) - 1)];
    }

    if (radioOff) bootloader_random_disable();
    return pass;
}

static bool validWifiPassword(const String& p) {
    return p.length() >= WIFI_PASSWORD_LENGTH;
}

static std::vector<Profile> profiles_;
static Settings              settings_;

static bool writeJsonAtomic(const char* path, const JsonDocument& doc) {
    const String tmpPath = String(path) + ".tmp";

    File f = LittleFS.open(tmpPath.c_str(), "w");
    if (!f) {
        Serial.printf("[profiles] Failed to open %s for write\n", tmpPath.c_str());
        return false;
    }

    const size_t expected = measureJson(doc);
    const size_t written  = serializeJson(doc, f);
    f.close();

    if (written != expected) {
        Serial.printf("[profiles] Short write to %s (%u of %u bytes) — discarding\n",
                      tmpPath.c_str(), (unsigned)written, (unsigned)expected);
        LittleFS.remove(tmpPath.c_str());
        return false;
    }

    if (!LittleFS.rename(tmpPath.c_str(), path)) {
        LittleFS.remove(path);
        if (!LittleFS.rename(tmpPath.c_str(), path)) {
            Serial.printf("[profiles] Failed to rename %s onto %s\n", tmpPath.c_str(), path);
            LittleFS.remove(tmpPath.c_str());
            return false;
        }
    }

    return true;
}

static Profile profileFromDefault(const DefaultProfile& d) {
    return Profile{
        String(d.name),
        Profiles::protocolFromString(String(d.protocol)),
        d.onCode,
        d.offCode,
        d.visible
    };
}

static std::vector<Profile> defaultProfiles() {
    std::vector<Profile> v;
    v.reserve(sizeof(DEFAULT_PROFILES) / sizeof(DEFAULT_PROFILES[0]));
    for (const auto& d : DEFAULT_PROFILES) v.push_back(profileFromDefault(d));
    return v;
}

static void profilesToDoc(const std::vector<Profile>& list, JsonDocument& doc) {
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& p : list) Profiles::toJson(p, arr.add<JsonObject>());
}

static void writeDefaultProfiles() {
    JsonDocument doc;
    profilesToDoc(defaultProfiles(), doc);
    writeJsonAtomic("/profiles.json", doc);
}

static bool writeSettings(const Settings& s) {
    JsonDocument doc;
    doc["active_profile"]    = s.activeProfile;
    doc["display_always_on"] = s.displayAlwaysOn;
    doc["wifi_password"]     = s.wifiPassword;
    return writeJsonAtomic("/settings.json", doc);
}

static void writeDefaultSettings() {
    Settings s = DEFAULT_SETTINGS;
    s.wifiPassword = generateWifiPassword();
    writeSettings(s);
}

static void loadProfiles() {
    profiles_.clear();

    File f = LittleFS.open("/profiles.json", "r");
    if (!f) { Serial.println("[profiles] profiles.json not found"); return; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[profiles] JSON parse error: %s\n", err.c_str());
        return;
    }

    for (JsonObjectConst obj : doc.as<JsonArrayConst>()) {
        if (profiles_.size() >= Profiles::MAX_PROFILES) {
            Serial.printf("[profiles] Ignoring profiles beyond the %u cap\n",
                          (unsigned)Profiles::MAX_PROFILES);
            break;
        }
        profiles_.push_back(Profiles::fromJson(obj));
    }

    Serial.printf("[profiles] Loaded %d profiles\n", (int)profiles_.size());
}

static void loadSettings() {
    settings_ = DEFAULT_SETTINGS;

    File f = LittleFS.open("/settings.json", "r");
    if (!f) { Serial.println("[profiles] settings.json not found"); return; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[profiles] Settings JSON parse error: %s\n", err.c_str());
        return;
    }

    settings_.activeProfile   = doc["active_profile"]    | DEFAULT_SETTINGS.activeProfile;
    settings_.displayAlwaysOn = doc["display_always_on"] | DEFAULT_SETTINGS.displayAlwaysOn;
    settings_.wifiPassword    = doc["wifi_password"]     | "";

    if (settings_.activeProfile < 0 ||
        settings_.activeProfile >= (int)profiles_.size()) {
        settings_.activeProfile = 0;
    }
}

namespace Profiles {
bool begin() {
    if (!LittleFS.begin(true)) {
        Serial.println("[profiles] LittleFS mount failed — running on fallback profile");
        // Never an open setup network: a password for this boot only.
        settings_ = DEFAULT_SETTINGS;
        settings_.wifiPassword = generateWifiPassword();
        return false;
    }

    if (!LittleFS.exists("/profiles.json")) {
        Serial.println("[profiles] First boot — writing default profiles");
        writeDefaultProfiles();
    }
    if (!LittleFS.exists("/settings.json")) {
        Serial.println("[profiles] First boot — writing default settings");
        writeDefaultSettings();
    }

    loadProfiles();

    if (profiles_.empty()) {
        Serial.println("[profiles] No profiles loaded — restoring defaults");
        writeDefaultProfiles();
        loadProfiles();
    }

    loadSettings();

    // Settings saved before the password existed, or a damaged file.
    if (!validWifiPassword(settings_.wifiPassword)) {
        Serial.println("[profiles] No WiFi password stored — generating one");
        settings_.wifiPassword = generateWifiPassword();
        writeSettings(settings_);
    }

    Serial.printf("[profiles] Active profile: %d (%s)\n",
                  settings_.activeProfile,
                  getActive().name.c_str());
    return !profiles_.empty();
}

const std::vector<Profile>& getAll()     { return profiles_; }

const Profile& getActive() {
    static const Profile FALLBACK = profileFromDefault(DEFAULT_PROFILES[0]);

    if (profiles_.empty()) return FALLBACK;

    if (settings_.activeProfile < 0 ||
        settings_.activeProfile >= (int)profiles_.size()) {
        return profiles_[0];
    }

    return profiles_[settings_.activeProfile];
}

const Settings&              getSettings()        { return settings_; }
Settings&                    getMutableSettings()  { return settings_; }

int nextVisibleIndex() {
    int n = (int)profiles_.size();
    if (n == 0) return 0;

    int start = settings_.activeProfile;

    for (int i = 1; i < n; i++) {
        int idx = (start + i) % n;
        if (profiles_[idx].visible) return idx;
    }

    return start;
}

void replaceAll(std::vector<Profile> newProfiles) {
    profiles_ = std::move(newProfiles);
    if (settings_.activeProfile < 0 ||
        settings_.activeProfile >= (int)profiles_.size()) {
        settings_.activeProfile = 0;
    }
}

void saveProfiles() {
    JsonDocument doc;
    profilesToDoc(profiles_, doc);

    if (writeJsonAtomic("/profiles.json", doc)) {
        Serial.println("[profiles] Saved profiles.json");
    }
}

void saveSettings() {
    if (writeSettings(settings_)) {
        Serial.println("[profiles] Saved settings.json");
    }
}

void factoryReset() {
    Serial.println("[profiles] Factory reset — restoring defaults");
    writeDefaultProfiles();
    writeDefaultSettings();
    loadProfiles();
    loadSettings();
    Serial.println("[profiles] Factory reset complete");
}

void toJson(const Profile& p, JsonObject obj) {
    char onBuf[11], offBuf[11];
    snprintf(onBuf,  sizeof(onBuf),  "0x%08" PRIX32, p.onCode);
    snprintf(offBuf, sizeof(offBuf), "0x%08" PRIX32, p.offCode);

    obj["name"]     = p.name;
    obj["protocol"] = protocolToString(p.protocol);
    obj["on"]       = onBuf;
    obj["off"]      = offBuf;
    obj["visible"]  = p.visible;
}

bool isConfigured(const Profile& p, bool on) {
    return (on ? p.onCode : p.offCode) != 0;
}

Profile fromJson(JsonObjectConst obj) {
    Profile p;
    p.name     = obj["name"].as<String>();
    p.protocol = protocolFromString(obj["protocol"].as<String>());

    const char* onStr  = obj["on"].as<const char*>();
    const char* offStr = obj["off"].as<const char*>();
    p.onCode   = onStr  ? (uint32_t)strtoul(onStr,  nullptr, 16) : 0;
    p.offCode  = offStr ? (uint32_t)strtoul(offStr, nullptr, 16) : 0;

    p.visible  = obj["visible"] | true;
    return p;
}

IrProtocol protocolFromString(const String& s) {
    if (s == "SAMSUNG") return IrProtocol::SAMSUNG;
    if (s == "SONY")    return IrProtocol::SONY;
    return IrProtocol::NEC;
}

String protocolToString(IrProtocol p) {
    switch (p) {
        case IrProtocol::SAMSUNG: return "SAMSUNG";
        case IrProtocol::SONY:    return "SONY";
        default:                  return "NEC";
    }
}

}
