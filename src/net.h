#pragma once

#include <Arduino.h>
#include <vector>

#include "settings.h"

enum class NetMode : uint8_t {
    Station,  // joined the configured Wi-Fi network
    SetupAp,  // no/failed credentials: running the "pixels2mqtt-setup" captive portal
};

// Joins the saved network, or falls back to the setup access point. Blocks for
// up to kConnectTimeoutMs while connecting.
void netBegin(const Settings& s);
void netLoop();

NetMode netMode();
String  netIp();
const std::vector<String>& netScanResults();  // SSIDs seen when the portal started

constexpr char kSetupApSsid[] = "pixels2mqtt-setup";
