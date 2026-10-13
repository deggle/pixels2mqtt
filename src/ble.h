#pragma once

// Pixels dice over BLE, entirely from adverts: die type, roll state, face and
// battery are all broadcast, so we never connect. A die is added the first
// time it's heard. State goes into the dice table (dice.h) and the LED (led.h).
//
// Protocol reference:
//   https://github.com/GameWithPixels/.github/blob/main/doc/CommunicationsProtocol.md
//   https://github.com/GameWithPixels/DiceFirmware (src/bluetooth/bluetooth_messages.h)

void bleBegin();
void bleLoop();  // prints BLE events and stats; call from loop()
