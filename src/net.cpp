#include "net.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

namespace {

// Generous: some routers reject the first attempt (AUTH_FAIL) and the retry
// succeeds; falling back to the portal too early looks like lost settings.
constexpr uint32_t kConnectTimeoutMs = 30000;
// If saved credentials exist but we fell back to the portal (e.g. the router
// was still booting after a power cut), reboot to retry once nobody is using it.
constexpr uint32_t kPortalRetryMs = 5 * 60 * 1000;

NetMode             g_mode = NetMode::SetupAp;
DNSServer           g_dns;
std::vector<String> g_scan;
bool                g_haveCredentials = false;
uint32_t            g_portalStartMs = 0;

void scanNetworks() {
    g_scan.clear();
    const int n = WiFi.scanNetworks();  // blocking, ~2-3 s
    for (int i = 0; i < n; ++i) {
        const String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;  // hidden network
        bool dup = false;
        for (const auto& s : g_scan) dup |= (s == ssid);
        if (!dup) g_scan.push_back(ssid);  // results come sorted by RSSI
    }
    WiFi.scanDelete();
}

void startSetupAp() {
    if (g_haveCredentials) WiFi.disconnect(true);  // drop the failed station attempt
    WiFi.mode(WIFI_AP_STA);  // STA half is only used for the scan
    scanNetworks();
    WiFi.softAP(kSetupApSsid);
    delay(100);
    g_dns.start(53, "*", WiFi.softAPIP());  // answer every lookup with our IP
    g_mode = NetMode::SetupAp;
    g_portalStartMs = millis();
    Serial.printf("Setup portal: join Wi-Fi \"%s\", then open http://%s/\n",
                  kSetupApSsid, WiFi.softAPIP().toString().c_str());
}

}  // namespace

void netBegin(const Settings& s) {
    WiFi.persistent(false);  // credentials live in our own Settings, not the Wi-Fi NVS
    WiFi.setHostname(s.deviceName.c_str());
    g_haveCredentials = s.wifiSsid.length() > 0;

    if (g_haveCredentials) {
        Serial.printf("Connecting to Wi-Fi \"%s\"...\n", s.wifiSsid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(s.wifiSsid.c_str(), s.wifiPass.c_str());
        const uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < kConnectTimeoutMs) {
            delay(100);
        }
        if (WiFi.status() == WL_CONNECTED) {
            g_mode = NetMode::Station;
            // Modem sleep delays incoming packets until the next DTIM beacon
            // (ping jitter of 13-218 ms observed). Mains powered, so keep the radio awake.
            Serial.printf("Wi-Fi power save off: %s\n", WiFi.setSleep(false) ? "ok" : "refused");
            if (MDNS.begin(s.deviceName.c_str())) {
                MDNS.addService("http", "tcp", 80);
            }
            Serial.printf("Wi-Fi connected: http://%s/ (http://%s.local/)\n",
                          WiFi.localIP().toString().c_str(), s.deviceName.c_str());
            return;
        }
        Serial.println("Wi-Fi connect failed");
    }
    startSetupAp();
}

void netLoop() {
    if (g_mode != NetMode::SetupAp) return;  // station mode reconnects by itself
    g_dns.processNextRequest();
    if (g_haveCredentials && WiFi.softAPgetStationNum() == 0 &&
        millis() - g_portalStartMs > kPortalRetryMs) {
        Serial.println("Portal idle with saved credentials: rebooting to retry Wi-Fi");
        ESP.restart();
    }
}

NetMode netMode() { return g_mode; }

String netIp() {
    return (g_mode == NetMode::Station ? WiFi.localIP() : WiFi.softAPIP()).toString();
}

const std::vector<String>& netScanResults() { return g_scan; }
