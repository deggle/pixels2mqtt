#pragma once

#include <Arduino.h>

// Per-die state shared between the BLE task (writer) and the web/main loop
// (readers). All access goes through these functions, which take a lock.

constexpr size_t kMaxDice = 16;

// Roll state values from the Pixels protocol (adverts and RollState messages)
enum : uint8_t {
    kRollUnknown  = 0,
    kRollRolled   = 1,  // settled after a roll: the final result
    kRollHandling = 2,
    kRollRolling  = 3,
    kRollCrooked  = 4,
    kRollOnFace   = 5,  // resting on a face without a roll (e.g. tipped or put down)
};

// Die type: advert design byte high nibble (also IAmADie byte 3). All values
// confirmed on hardware (the d4 has 6 LEDs, so never infer type from LED count).
enum : uint8_t {
    kDieUnknown = 0,
    kDieD4      = 1,
    kDieD6      = 2,
    kDieD8      = 3,
    kDieD10     = 4,
    kDieD00     = 5,  // percentile: 10 LEDs like a d10, faces read x10
    kDieD12     = 6,
    kDieD20     = 7,
};

// Value shown on the die for a 0-based face index. d10: index, 0 = 10.
// d00: index x 10, 0 = 100. Others: index + 1.
uint8_t dieFaceValue(uint8_t dieType, uint8_t face);
// "d00", "d20", ... from the type, falling back to the LED count ("d10?" style)
String  dieLabel(uint8_t dieType, uint8_t ledCount);

struct DieState {
    bool     used;          // slot assigned to a die this boot
    bool     online;        // advert heard within kDieOfflineMs
    char     name[24];      // advertised name
    char     address[18];   // BLE MAC, "aa:bb:cc:dd:ee:ff"
    uint32_t pixelId;
    uint8_t  ledCount;
    uint8_t  dieType;       // from the advert design byte (high nibble)
    uint8_t  rollState;
    uint8_t  face;          // 0-based face index (0 = shows 1)
    uint8_t  battery;       // percent
    bool     charging;
    int8_t   rssi;          // last advert, dBm
    uint8_t  lastRollFace;  // 0-based face of the last completed roll (valid if rollCount > 0)
    uint32_t rollCount;     // increments on each completed roll
    uint32_t lastSeenMs;
};

constexpr uint32_t kDieOfflineMs = 15000;

// Finds the slot for this address, or assigns a free one. -1 if the table is full.
int diceSlotFor(const char* address, const char* name);

// Applies one advert. Only bumps the change counter when something the UI
// shows has changed (RSSI and last-seen ride along with the next real change).
// Returns the previous roll state, so the caller can spot transitions.
// pixelId 0 = not in this packet (keeps the last known one).
uint8_t diceApplyAdvert(int slot, uint32_t pixelId, uint8_t ledCount, uint8_t dieType,
                        uint8_t rollState, uint8_t face, uint8_t battery, bool charging,
                        int8_t rssi);

// Roll group: starts when a die starts rolling with no group open; every die
// that starts rolling while it's open joins; ends when no member is rolling.
// Each member's value is recorded when it lands face up (rolled or on face);
// re-rolling clears it, picking it up afterwards keeps it. Crooked or caught
// mid-tumble = no value (not counted).
struct RollGroup {
    bool     active;              // members still rolling
    uint32_t seq;                 // increments each time a group ends
    uint32_t members;             // bit per dice slot
    uint8_t  value[kMaxDice];     // landed value per member, 0 = none (values are >= 1)
};

void diceRollGroup(RollGroup& out);
uint16_t rollGroupTotal(const RollGroup& g, uint8_t* landed = nullptr, uint8_t* count = nullptr);

// Marks dice offline when no advert has arrived for kDieOfflineMs. Call from loop().
void diceExpire();

// Copies the whole table. Returns the change counter at the time of the copy.
uint32_t diceSnapshot(DieState out[kMaxDice]);
uint32_t diceVersion();
