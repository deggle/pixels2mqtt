#pragma once

#include <Arduino.h>

// NanoC6 WS2812 status LED. Blue while any die is rolling, green flash when
// one settles, red flash if it lands crooked, dim magenta in setup mode.
// Event functions are safe to call from the BLE task; ledUpdate() runs in loop().

void ledInit();
void ledUpdate();
void ledSet(uint8_t r, uint8_t g, uint8_t b);  // direct write (bypasses state)

void ledOnRollState(int slot, uint8_t state);

constexpr uint8_t kLedBrightness = 40;  // WS2812 is very bright at full
