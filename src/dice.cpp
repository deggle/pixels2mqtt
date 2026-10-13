#include "dice.h"

#include <freertos/FreeRTOS.h>

#include <atomic>

namespace {

DieState              g_dice[kMaxDice] = {};
std::atomic<uint32_t> g_version{0};
portMUX_TYPE          g_lock = portMUX_INITIALIZER_UNLOCKED;

// Critical sections must stay short: no heap, no logging, no blocking.
struct Lock {
    Lock()  { portENTER_CRITICAL(&g_lock); }
    ~Lock() { portEXIT_CRITICAL(&g_lock); }
};

bool valid(int slot) { return slot >= 0 && slot < (int)kMaxDice; }

RollGroup g_group = {};

// Call with the lock held. Closes the group once no member is rolling (dice
// that went asleep mid-roll count as stopped). Returns true if it closed.
bool maybeEndGroup() {
    if (!g_group.active) return false;
    for (int i = 0; i < (int)kMaxDice; ++i) {
        if ((g_group.members & (1u << i)) && g_dice[i].online && g_dice[i].rollState == kRollRolling) {
            return false;
        }
    }
    g_group.active = false;
    ++g_group.seq;
    return true;
}

// Call with the lock held, after the die's state is updated. Returns true if
// the group changed in a way the UI shows.
bool updateGroup(int slot, uint8_t prevState) {
    const DieState& d = g_dice[slot];
    const uint32_t bit = 1u << slot;
    if (d.rollState == kRollRolling) {
        if (!g_group.active) {
            g_group.active = true;
            g_group.members = 0;
            memset(g_group.value, 0, sizeof(g_group.value));
        }
        if (prevState == kRollRolling && (g_group.members & bit)) return false;
        g_group.members |= bit;
        g_group.value[slot] = 0;  // (re-)rolling: forget any earlier landing
        return true;
    }
    bool changed = false;
    if (g_group.active && (g_group.members & bit) && !g_group.value[slot] &&
        (d.rollState == kRollRolled || d.rollState == kRollOnFace)) {
        g_group.value[slot] = dieFaceValue(d.dieType, d.face);
        changed = true;
    }
    return maybeEndGroup() || changed;
}

}  // namespace

uint16_t rollGroupTotal(const RollGroup& g, uint8_t* landed, uint8_t* count) {
    uint16_t total = 0;
    uint8_t n = 0, members = 0;
    for (size_t i = 0; i < kMaxDice; ++i) {
        if (!(g.members & (1u << i))) continue;
        ++members;
        if (g.value[i]) { total += g.value[i]; ++n; }
    }
    if (landed) *landed = n;
    if (count) *count = members;
    return total;
}

void diceRollGroup(RollGroup& out) {
    portENTER_CRITICAL(&g_lock);
    out = g_group;
    portEXIT_CRITICAL(&g_lock);
}

uint8_t dieFaceValue(uint8_t dieType, uint8_t face) {
    // d10 and d00 faces are labelled 0-9 (x10 on the d00); index 0 is the top value
    if (dieType == kDieD10) return face == 0 ? 10 : face;
    if (dieType == kDieD00) return face == 0 ? 100 : face * 10;
    return face + 1;
}

String dieLabel(uint8_t dieType, uint8_t ledCount) {
    switch (dieType) {
        case kDieD4:  return "d4";
        case kDieD6:  return "d6";
        case kDieD8:  return "d8";
        case kDieD10: return "d10";
        case kDieD00: return "d00";
        case kDieD12: return "d12";
        case kDieD20: return "d20";
    }
    return ledCount ? "d" + String(ledCount) + "?" : String();  // not identified yet
}

int diceSlotFor(const char* address, const char* name) {
    Lock lock;
    int freeSlot = -1;
    for (int i = 0; i < (int)kMaxDice; ++i) {
        if (g_dice[i].used && strcmp(g_dice[i].address, address) == 0) {
            // First advert may lack the name (it's in the scan response)
            if (name && *name && strcmp(g_dice[i].name, address) == 0) {
                strlcpy(g_dice[i].name, name, sizeof(g_dice[i].name));
                ++g_version;
            }
            return i;
        }
        if (!g_dice[i].used && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot >= 0) {
        DieState& d = g_dice[freeSlot];
        d = DieState{};
        d.used = true;
        strlcpy(d.address, address, sizeof(d.address));
        strlcpy(d.name, (name && *name) ? name : address, sizeof(d.name));
        ++g_version;
    }
    return freeSlot;
}

uint8_t diceApplyAdvert(int slot, uint32_t pixelId, uint8_t ledCount, uint8_t dieType,
                        uint8_t rollState, uint8_t face, uint8_t battery, bool charging,
                        int8_t rssi) {
    if (!valid(slot)) return kRollUnknown;
    Lock lock;
    DieState& d = g_dice[slot];
    const uint8_t prevState = d.rollState;
    if (pixelId == 0) pixelId = d.pixelId;  // only scan responses carry it
    const bool changed = !d.online || d.pixelId != pixelId || d.ledCount != ledCount || d.dieType != dieType ||
                         d.rollState != rollState || d.face != face ||
                         d.battery != battery || d.charging != charging;
    // A new result: entering "rolled", or a different face while still "rolled"
    // (the "rolling" adverts in between were missed).
    const bool newRoll = rollState == kRollRolled &&
                         (prevState != kRollRolled || d.face != face) && d.online;
    d.online = true;
    d.pixelId = pixelId;
    d.ledCount = ledCount;
    d.dieType = dieType;
    d.rollState = rollState;
    d.face = face;
    d.battery = battery;
    d.charging = charging;
    d.rssi = rssi;
    d.lastSeenMs = millis();
    if (newRoll) {
        d.lastRollFace = face;
        ++d.rollCount;
    }
    if (updateGroup(slot, prevState) || changed) ++g_version;
    return prevState;
}

void diceExpire() {
    const uint32_t now = millis();
    Lock lock;
    for (auto& d : g_dice) {
        if (d.used && d.online && now - d.lastSeenMs > kDieOfflineMs) {
            d.online = false;
            ++g_version;
        }
    }
    if (maybeEndGroup()) ++g_version;  // a member fell asleep mid-roll
}

uint32_t diceSnapshot(DieState out[kMaxDice]) {
    Lock lock;
    memcpy(out, g_dice, sizeof(g_dice));
    return g_version;
}

uint32_t diceVersion() { return g_version; }
