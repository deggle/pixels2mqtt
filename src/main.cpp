// pixels2mqtt: ESP32-C6 (M5Stack NanoC6) gateway for Pixels electronic dice.
//
// ble.*      listens to Pixels adverts (never connects)
// dice.*     per-die state shared between BLE and web
// led.*      status LED
// net.*      Wi-Fi station / setup access point
// web.*      settings page, live dashboard over WebSocket
// mqtt.*     MQTT publishing and Home Assistant discovery
// settings.* persisted settings

#include <Arduino.h>

#include "ble.h"
#include "dice.h"
#include "led.h"
#include "mqtt.h"
#include "net.h"
#include "settings.h"
#include "web.h"

namespace {

Settings g_settings;

// ---- Button: press = reboot, hold 5 s = clear settings ---------------------------
// Both act on release: GPIO9 is the boot strapping pin, so rebooting while it's
// still held low risks starting the ROM bootloader instead of the firmware.
// LED while held: dim red after 1 s ("keep holding to clear"), fast red blink
// once settings are cleared ("release to reboot").

constexpr uint32_t kDebounceMs  = 50;
constexpr uint32_t kHoldHintMs  = 1000;
constexpr uint32_t kResetHoldMs = 5000;

// Returns true while the button is driving the LED
bool buttonLoop() {
    // Keep "pressed" as its own flag. An earlier version marked it by forcing
    // the timestamp odd (now | 1), which made held = now - (now + 1) wrap to
    // ~4.29e9 ms on even timestamps and cleared settings on a short press.
    static bool     down = false;
    static uint32_t pressedAt = 0;
    static bool     cleared = false;
    const uint32_t  now = millis();

    if (digitalRead(BTN_PIN) == LOW) {  // active low
        if (!down) {
            down = true;
            pressedAt = now;
        }
        const uint32_t held = now - pressedAt;
        if (held >= kResetHoldMs) {
            if (!cleared) {
                cleared = true;
                settingsClear();
                Serial.println("Button held: settings cleared, release to reboot");
            }
            ledSet((now / 150) % 2 ? kLedBrightness : 0, 0, 0);
            return true;
        }
        if (held >= kHoldHintMs) {
            ledSet(kLedBrightness / 4, 0, 0);
            return true;
        }
        return false;
    }

    if (down) {  // released
        down = false;
        const uint32_t held = now - pressedAt;
        if (cleared || held >= kDebounceMs) {
            Serial.println(cleared ? "Rebooting into setup mode" : "Button pressed: rebooting");
            ledSet(0, 0, 0);
            delay(100);  // let the log line out
            ESP.restart();
        }
    }
    return false;
}

}  // namespace

void setup() {
    Serial.begin(115200);
    // Never block on USB serial: with the board plugged in but no monitor
    // reading, each write would otherwise wait for the TX timeout.
    Serial.setTxTimeoutMs(0);
    delay(1000);  // give USB-CDC time to enumerate
    Serial.println("\npixels2mqtt starting");
    ledInit();
    pinMode(BTN_PIN, INPUT_PULLUP);

    settingsLoad(g_settings);
    netBegin(g_settings);
    webBegin(g_settings);
    if (netMode() == NetMode::Station) {
        mqttBegin(g_settings);
        bleBegin();
    } else {
        // No dice scanning in setup mode: a continuous BLE scan shares the radio
        // with the access point and starves it (phones join but the portal won't load)
        Serial.println("Setup mode: BLE off until Wi-Fi is configured");
    }
}

void loop() {
    static uint32_t lastExpireMs = 0;
    if (millis() - lastExpireMs >= 1000) {
        lastExpireMs = millis();
        diceExpire();  // dice we haven't heard from go "asleep"
    }
    bleLoop();
    netLoop();
    webLoop();
    if (!buttonLoop()) ledUpdate();
    delay(10);
}
