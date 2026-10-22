#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

enum class IrProtocol {
    NEC,
    SAMSUNG,
    SONY
};

struct Profile {
    String     name;
    IrProtocol protocol;
    uint32_t   onCode;
    uint32_t   offCode;
    bool       visible;
};

struct Settings {
    int    activeProfile;
    bool   displayAlwaysOn;
    // The setup network's password, random per stick and shown on the screen.
    String wifiPassword;
};

namespace Profiles {
    static constexpr size_t MAX_PROFILES = 32;

    bool begin();

    const std::vector<Profile>& getAll();
    const Profile&               getActive();
    const Settings&              getSettings();

    Settings& getMutableSettings();

    void replaceAll(std::vector<Profile> newProfiles);

    int nextVisibleIndex();

    void saveProfiles();
    void saveSettings();

    void factoryReset();

    Profile fromJson(JsonObjectConst obj);

    void toJson(const Profile& p, JsonObject obj);

    bool isConfigured(const Profile& p, bool on);

    IrProtocol protocolFromString(const String& s);
    String     protocolToString(IrProtocol p);

}
