#include "settings.h"

#include <Preferences.h>

namespace {
constexpr char kNamespace[] = "p2m";
}

void settingsLoad(Settings& s) {
    Preferences prefs;
    // Read-write so a fresh device creates the namespace instead of logging
    // a NOT_FOUND error; missing keys fall back to the defaults below.
    prefs.begin(kNamespace, false);
    const Settings defaults;
    // getString() logs an error for a missing key, so check first
    auto str = [&](const char* key, const String& def) {
        return prefs.isKey(key) ? prefs.getString(key, def) : def;
    };
    s.wifiSsid   = str("wifiSsid", defaults.wifiSsid);
    s.wifiPass   = str("wifiPass", defaults.wifiPass);
    s.deviceName = str("devName", defaults.deviceName);
    s.mqttHost   = str("mqttHost", defaults.mqttHost);
    s.mqttPort   = prefs.getUShort("mqttPort", defaults.mqttPort);
    s.mqttUser   = str("mqttUser", defaults.mqttUser);
    s.mqttPass   = str("mqttPass", defaults.mqttPass);
    s.mqttUidTopics = prefs.getBool("mqttUid", defaults.mqttUidTopics);
    s.haDiscovery   = prefs.getBool("haDisc", defaults.haDiscovery);
    prefs.end();
}

void settingsSave(const Settings& s) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putString("wifiSsid", s.wifiSsid);
    prefs.putString("wifiPass", s.wifiPass);
    prefs.putString("devName", s.deviceName);
    prefs.putString("mqttHost", s.mqttHost);
    prefs.putUShort("mqttPort", s.mqttPort);
    prefs.putString("mqttUser", s.mqttUser);
    prefs.putString("mqttPass", s.mqttPass);
    prefs.putBool("mqttUid", s.mqttUidTopics);
    prefs.putBool("haDisc", s.haDiscovery);
    prefs.end();
}

void settingsClear() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.clear();
    prefs.end();
}
