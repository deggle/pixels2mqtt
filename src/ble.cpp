#include "ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <atomic>

#include "dice.h"
#include "led.h"

namespace {

// Manufacturer data (7 bytes): company id (2, 0xFFFF), ledCount,
// design (die type << 4 | colourway), rollState, currentFace,
// battery (bit 7 = charging, bits 0-6 = percent).
// Service data 0x180a (scan response only): pixelId (4, LE), buildTimestamp (4).
constexpr size_t kMfrLen = 7;

constexpr uint32_t kStatsIntervalMs = 5000;

// Events for serial logging, produced in the BLE host task and printed from
// loop(). The host task must never block: a stalled host makes the controller
// discard adverts, which is what lost rolls when all the dice were rolled at once.
struct Event {
    uint32_t ms;
    uint8_t  slot;
    bool     isNew;            // first advert from this die
    uint8_t  mfr[kMfrLen];
};

QueueHandle_t g_events = nullptr;

// Stats, reset each kStatsIntervalMs
std::atomic<uint32_t> g_advertsAll{0};     // every advert from every device
std::atomic<uint32_t> g_advertsPixels{0};  // Pixels adverts with state
std::atomic<uint32_t> g_handlerMaxUs{0};
std::atomic<uint32_t> g_eventsDropped{0};

bool g_seen[kMaxDice] = {};  // BLE task only

// ---- Adverts (NimBLE host task) ----------------------------------------------
//
// With active scanning NimBLE calls onResult only once a scan response arrives,
// and the controller backs off scan requests, so that alone gave ~10 s updates.
// onDiscovered fires for every advert (duplicates allowed) and carries the
// manufacturer data (state); the scan response adds the name and pixel ID,
// which we only need once. Both feed the same handler.

class ScanCallbacks : public NimBLEScanCallbacks {
    void onDiscovered(const NimBLEAdvertisedDevice* dev) override { handle(dev); }
    void onResult(const NimBLEAdvertisedDevice* dev) override { handle(dev); }

    void handle(const NimBLEAdvertisedDevice* dev) {
        static const NimBLEUUID kPixelsService("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
        ++g_advertsAll;
        const uint32_t t0 = micros();

        if (!dev->isAdvertisingService(kPixelsService)) return;
        const std::string mfr = dev->getManufacturerData();
        if (mfr.size() < kMfrLen) return;  // no state in this packet
        ++g_advertsPixels;

        const int slot = diceSlotFor(dev->getAddress().toString().c_str(), dev->getName().c_str());
        if (slot < 0) return;  // table full

        uint32_t pixelId = 0;  // 0 = not in this packet; the table keeps the last one
        if (dev->getServiceDataCount()) {
            const std::string sd = dev->getServiceData(0);
            if (sd.size() >= 4) memcpy(&pixelId, sd.data(), 4);
        }

        const uint8_t dieType = uint8_t(mfr[3]) >> 4;
        const uint8_t state = mfr[4], face = mfr[5], batt = mfr[6];
        const uint8_t prev = diceApplyAdvert(slot, pixelId, mfr[2], dieType, state, face,
                                             batt & 0x7F, batt & 0x80, dev->getRSSI());

        const bool isNew = !g_seen[slot];
        g_seen[slot] = true;
        if (isNew || state != prev) {
            if (!isNew) ledOnRollState(slot, state);
            Event e{millis(), (uint8_t)slot, isNew, {}};
            memcpy(e.mfr, mfr.data(), kMfrLen);
            if (xQueueSend(g_events, &e, 0) != pdTRUE) ++g_eventsDropped;
        }

        const uint32_t us = micros() - t0;
        uint32_t max = g_handlerMaxUs;
        while (us > max && !g_handlerMaxUs.compare_exchange_weak(max, us)) {}
    }
};

ScanCallbacks g_scanCallbacks;

}  // namespace

void bleBegin() {
    g_events = xQueueCreate(64, sizeof(Event));

    NimBLEDevice::init("pixels2mqtt");

    NimBLEScan* scan = NimBLEDevice::getScan();
    // Duplicates on: every advert carries fresh state, so we need them all.
    scan->setScanCallbacks(&g_scanCallbacks, true);
    scan->setMaxResults(0);  // don't store results, we act in the callback
    // Active so the scan response delivers each die's name and pixel ID
    scan->setActiveScan(true);
    // Listen continuously (window = interval); the coexistence scheduler still
    // gives Wi-Fi its slots. A 75% duty cycle missed adverts when all dice rolled.
    scan->setInterval(48);
    scan->setWindow(48);
    scan->start(0, false, true);
    Serial.println("Listening for Pixels dice adverts...");
}

void bleLoop() {
    if (!g_events) return;  // BLE not started (setup mode)
    Event e;
    DieState dice[kMaxDice];
    bool haveSnapshot = false;
    while (xQueueReceive(g_events, &e, 0) == pdTRUE) {
        if (!haveSnapshot) { diceSnapshot(dice); haveSnapshot = true; }
        const DieState& d = dice[e.slot];
        const char* who = d.name;
        if (e.isNew) {
            Serial.printf("%8lu ms New die %s (%s) %s mfr:", e.ms, who, d.address,
                          dieLabel(e.mfr[3] >> 4, e.mfr[2]).c_str());
            for (uint8_t b : e.mfr) Serial.printf(" %02X", b);
            Serial.println();
        } else {
            Serial.printf("%8lu ms [%s %s] state %u face %u\n", e.ms,
                          dieLabel(d.dieType, d.ledCount).c_str(), who, e.mfr[4], e.mfr[5]);
        }
    }

    // Roll group finished: log the total (MQTT will hook in here too)
    static uint32_t lastGroupSeq = 0;
    {
        RollGroup g;
        diceRollGroup(g);
        if (g.seq != lastGroupSeq && !g.active) {
            lastGroupSeq = g.seq;
            if (!haveSnapshot) { diceSnapshot(dice); haveSnapshot = true; }
            uint8_t landed, count;
            const uint16_t total = rollGroupTotal(g, &landed, &count);
            Serial.printf("%8lu ms Roll total: %u (%u of %u dice):", millis(), total, landed, count);
            for (size_t i = 0; i < kMaxDice; ++i) {
                if (!(g.members & (1u << i))) continue;
                Serial.printf(" %s=%s", dieLabel(dice[i].dieType, dice[i].ledCount).c_str(),
                              g.value[i] ? String(g.value[i]).c_str() : "no result");
            }
            Serial.println();
        }
    }

    static uint32_t lastStatsMs = 0;
    const uint32_t now = millis();
    if (now - lastStatsMs >= kStatsIntervalMs) {
        const float secs = (now - lastStatsMs) / 1000.0f;
        lastStatsMs = now;
        Serial.printf("BLE: %.0f adverts/s (%.0f Pixels), slowest handler %lu us, log events dropped %lu\n",
                      g_advertsAll.exchange(0) / secs, g_advertsPixels.exchange(0) / secs,
                      (unsigned long)g_handlerMaxUs.exchange(0), (unsigned long)g_eventsDropped.exchange(0));
    }
}
