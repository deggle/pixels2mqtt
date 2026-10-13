#pragma once

#include "settings.h"

// Publishes dice events to MQTT with plain payloads. <die> is the die type
// (d6) or, with mqttUidTopics, the pixel ID (DB0605FC).
//
//   pixels/<die>/rolled   value, e.g. 17     each completed roll
//   pixels/<die>/rolling  1 / 0              starts / stops tumbling
//   pixels/<die>/battery  0-100 (retained)   on change
//   pixels/total          sum (see dice.h)   when a roll group ends
//   pixels/status         online / offline   retained; offline is the last will
//
// With haDiscovery, each die becomes a Home Assistant device (Roll sensor,
// Roll event, Rolling binary sensor, Battery) via the gateway, which carries
// Roll total sensor and event. Configs are retained and re-sent when HA
// announces itself on homeassistant/status.
//
// Uses ESP-IDF's esp-mqtt (own task, reconnects by itself, keeps QoS 1
// messages while the broker is unreachable). Changes are published from a
// separate task that watches the dice table, so loop() never blocks on MQTT.

enum class MqttState : uint8_t { Off, Connecting, Connected };

void      mqttBegin(const Settings& s);  // no-op if no broker host is set
MqttState mqttState();
