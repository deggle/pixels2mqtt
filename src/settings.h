#pragma once

#include <Arduino.h>

// User settings, persisted in NVS (survive reboot and reflash, cleared by a
// 5 s button hold).
struct Settings {
    String   wifiSsid;
    String   wifiPass;
    String   deviceName = "pixels2mqtt";  // also the hostname / mDNS name
    String   mqttHost;                    // empty = MQTT disabled
    uint16_t mqttPort = 1883;
    String   mqttUser;
    String   mqttPass;
    bool     mqttUidTopics = false;       // pixels/<uid>/... instead of pixels/d6/...
    bool     haDiscovery = false;         // publish Home Assistant discovery configs
};

void settingsLoad(Settings& s);
void settingsSave(const Settings& s);
void settingsClear();
