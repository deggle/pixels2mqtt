#include "led.h"

#include <atomic>

#include "dice.h"
#include "net.h"

namespace {

constexpr uint32_t kFlashMs       = 400;
constexpr uint32_t kRollTimeoutMs = 10000;  // clear "rolling" if we miss the settle

enum class Flash : uint8_t { None, Green, Red };

std::atomic<uint32_t> g_rollingMask{0};  // bit per dice slot
std::atomic<uint32_t> g_rollStartMs{0};
std::atomic<Flash>    g_pendingFlash{Flash::None};

}  // namespace

void ledInit() {
    pinMode(RGB_LED_PWR_PIN, OUTPUT);
    digitalWrite(RGB_LED_PWR_PIN, HIGH);
    rgbLedWrite(RGB_LED_DATA_PIN, 0, 0, 0);
}

void ledSet(uint8_t r, uint8_t g, uint8_t b) {
    static uint32_t last = 0xFFFFFFFF;
    const uint32_t rgb = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    if (rgb == last) return;  // avoid re-sending the same colour every loop
    last = rgb;
    rgbLedWrite(RGB_LED_DATA_PIN, r, g, b);
}

void ledOnRollState(int slot, uint8_t state) {
    const uint32_t bit = 1u << slot;
    if (state == kRollRolling) {
        g_rollingMask |= bit;
        g_rollStartMs = millis();
        return;
    }
    const bool wasRolling = g_rollingMask.fetch_and(~bit) & bit;
    if (state == kRollRolled) {
        g_pendingFlash = Flash::Green;
    } else if (state == kRollCrooked && wasRolling) {
        g_pendingFlash = Flash::Red;
    }
    // handling / onFace / unknown: just stop showing "rolling"
}

void ledUpdate() {
    static Flash    flash = Flash::None;
    static uint32_t flashStart = 0;
    const uint32_t now = millis();

    const Flash pending = g_pendingFlash.exchange(Flash::None);
    if (pending != Flash::None) {
        flash = pending;
        flashStart = now;
    }
    if (g_rollingMask && now - g_rollStartMs > kRollTimeoutMs) {
        g_rollingMask = 0;
    }

    if (flash != Flash::None && now - flashStart < kFlashMs) {
        if (flash == Flash::Green) ledSet(0, kLedBrightness, 0);
        else                       ledSet(kLedBrightness, 0, 0);
    } else {
        flash = Flash::None;
        if (g_rollingMask)                      ledSet(0, 0, kLedBrightness);
        else if (netMode() == NetMode::SetupAp) ledSet(6, 0, 6);  // dim magenta: setup portal
        else                                    ledSet(0, 0, 0);
    }
}
