#include "mqtt.h"

#include <WiFi.h>
#include <mqtt_client.h>

#include <atomic>

#include "dice.h"

namespace {

constexpr char kBase[]         = "pixels";
constexpr char kStatusTopic[]  = "pixels/status";
constexpr char kTotalTopic[]   = "pixels/total";
constexpr char kHaPrefix[]     = "homeassistant";
constexpr char kHaStatus[]     = "homeassistant/status";

const Settings*          g_s = nullptr;
esp_mqtt_client_handle_t g_client = nullptr;
std::atomic<MqttState>   g_state{MqttState::Off};
std::atomic<bool>        g_republish{false};  // (re)connected, or HA restarted
String                   g_gatewayId;         // "pixels2mqtt_<mac>"

// What we've published per dice slot, to publish only changes
struct Published {
    bool     seen;
    uint32_t rollCount;
    bool     rolling;
    int16_t  battery;      // -1 = force resend
    bool     discovered;
};
Published g_pub[kMaxDice] = {};
uint32_t  g_lastVersion = UINT32_MAX;
uint32_t  g_lastGroupSeq = 0;

// Called only from the publisher task. publish() writes straight to the socket
// (enqueue() would leave each message waiting for esp-mqtt's 1 s poll loop, one
// message per pass). While disconnected, QoS 1 messages are kept in the outbox.
void publish(const String& topic, const String& payload, bool retain) {
    esp_mqtt_client_publish(g_client, topic.c_str(), payload.c_str(), payload.length(), 1, retain);
}

String uidOf(const DieState& d) {
    char id[9];
    snprintf(id, sizeof(id), "%08lX", (unsigned long)d.pixelId);
    return id;
}

// Topic segment for a die, or "" if we can't name it yet
String dieTopicId(const DieState& d) {
    if (g_s->mqttUidTopics) return d.pixelId ? uidOf(d) : String();
    return d.dieType != kDieUnknown ? dieLabel(d.dieType, d.ledCount) : String();
}

// ---- Home Assistant discovery -------------------------------------------------

String haAvailability() {
    return String(R"(,"availability_topic":")") + kStatusTopic + R"(")";
}

void haPublishGateway() {
    const String dev = String(R"({"identifiers":[")") + g_gatewayId + R"("],"name":")" + g_s->deviceName +
                       R"(","model":"M5Stack NanoC6","manufacturer":"pixels2mqtt"})";
    const String base = String(kHaPrefix) + "/";
    publish(base + "sensor/" + g_gatewayId + "/total/config",
            String(R"({"name":"Roll total","unique_id":")") + g_gatewayId + R"(_total","state_topic":")" +
                kTotalTopic + R"(","force_update":true,"icon":"mdi:sigma")" + haAvailability() +
                R"(,"device":)" + dev + "}",
            true);
    publish(base + "event/" + g_gatewayId + "/total_event/config",
            String(R"({"name":"Total event","unique_id":")") + g_gatewayId + R"(_total_event","state_topic":")" +
                kTotalTopic + R"(","event_types":["total"],"value_template":"{\"event_type\":\"total\",\"value\":{{ value }}}")" +
                haAvailability() + R"(,"device":)" + dev + "}",
            true);
}

void haPublishDie(const DieState& d, const String& topicId) {
    const String uid = uidOf(d);
    const String label = dieLabel(d.dieType, d.ledCount);
    const String node = "pixels_" + uid;
    const String t = String(kBase) + "/" + topicId + "/";
    const String dev = String(R"({"identifiers":[")") + node + R"("],"name":"Pixels )" + label + R"(","model":")" +
                       label + R"(","serial_number":")" + uid +
                       R"(","manufacturer":"Systemic Games","via_device":")" + g_gatewayId + R"("})";
    const String tail = haAvailability() + R"(,"device":)" + dev + "}";
    const String base = String(kHaPrefix) + "/";

    publish(base + "sensor/" + node + "/roll/config",
            String(R"({"name":"Roll","unique_id":")") + node + R"(_roll","state_topic":")" + t +
                R"(rolled","force_update":true,"icon":"mdi:dice-multiple")" + tail,
            true);
    publish(base + "event/" + node + "/roll_event/config",
            String(R"({"name":"Roll event","unique_id":")") + node + R"(_roll_event","state_topic":")" + t +
                R"(rolled","event_types":["rolled"],"value_template":"{\"event_type\":\"rolled\",\"value\":{{ value }}}")" +
                tail,
            true);
    publish(base + "binary_sensor/" + node + "/rolling/config",
            String(R"({"name":"Rolling","unique_id":")") + node + R"(_rolling","state_topic":")" + t +
                R"(rolling","payload_on":"1","payload_off":"0","icon":"mdi:rotate-3d-variant")" + tail,
            true);
    publish(base + "sensor/" + node + "/battery/config",
            String(R"({"name":"Battery","unique_id":")") + node + R"(_battery","state_topic":")" + t +
                R"(battery","device_class":"battery","unit_of_measurement":"%","state_class":"measurement","entity_category":"diagnostic")" +
                tail,
            true);
}

// ---- esp-mqtt events (MQTT task) ---------------------------------------------

void onEvent(void*, esp_event_base_t, int32_t id, void* data) {
    auto* e = static_cast<esp_mqtt_event_handle_t>(data);
    switch (id) {
        case MQTT_EVENT_CONNECTED:
            g_state = MqttState::Connected;
            g_republish = true;
            if (g_s->haDiscovery) esp_mqtt_client_subscribe(g_client, kHaStatus, 1);
            break;
        case MQTT_EVENT_DISCONNECTED:
            g_state = MqttState::Connecting;
            break;
        case MQTT_EVENT_DATA:
            // HA restarted: it forgets non-retained state, and wants configs again
            if (e->topic_len == (int)strlen(kHaStatus) && strncmp(e->topic, kHaStatus, e->topic_len) == 0 &&
                e->data_len == 6 && strncmp(e->data, "online", 6) == 0) {
                g_republish = true;
            }
            break;
        default:
            break;
    }
}

// One pass: publish whatever changed in the dice table since last time
void publishChanges() {
    if (g_republish.exchange(false)) {
        publish(kStatusTopic, "online", true);
        if (g_s->haDiscovery) haPublishGateway();
        for (auto& p : g_pub) { p.battery = -1; p.discovered = false; }
        g_lastVersion = UINT32_MAX;  // force a pass over the table
    }

    if (diceVersion() == g_lastVersion) return;
    DieState dice[kMaxDice];
    g_lastVersion = diceSnapshot(dice);

    for (size_t i = 0; i < kMaxDice; ++i) {
        const DieState& d = dice[i];
        if (!d.used) continue;
        const String id = dieTopicId(d);
        if (id.isEmpty()) continue;  // type or pixel ID not known yet
        Published& p = g_pub[i];
        const String t = String(kBase) + "/" + id + "/";

        if (!p.seen) {  // baseline: don't replay rolls from before we were watching
            p.seen = true;
            p.rollCount = d.rollCount;
            p.rolling = false;
            p.battery = -1;
        }
        // rolling before rolled, so subscribers see "stopped" then the result
        const bool rolling = d.online && d.rollState == kRollRolling;
        if (rolling != p.rolling) {
            p.rolling = rolling;
            publish(t + "rolling", rolling ? "1" : "0", false);
        }
        if (d.rollCount != p.rollCount) {
            p.rollCount = d.rollCount;
            publish(t + "rolled", String(dieFaceValue(d.dieType, d.lastRollFace)), false);
        }
        if (d.battery != p.battery) {
            p.battery = d.battery;
            publish(t + "battery", String(d.battery), true);
        }
        if (g_s->haDiscovery && !p.discovered && d.pixelId && d.dieType != kDieUnknown) {
            p.discovered = true;
            haPublishDie(d, id);
        }
    }

    RollGroup g;
    diceRollGroup(g);
    if (!g.active && g.seq != g_lastGroupSeq) {
        g_lastGroupSeq = g.seq;
        publish(kTotalTopic, String(rollGroupTotal(g)), false);
    }
}

// Own task, so a slow socket write never stalls loop() (LED, web, button)
void publisherTask(void*) {
    for (;;) {
        publishChanges();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

void mqttBegin(const Settings& s) {
    g_s = &s;
    if (s.mqttHost.isEmpty()) return;

    String mac = WiFi.macAddress();
    mac.replace(":", "");
    mac.toLowerCase();
    g_gatewayId = "pixels2mqtt_" + mac;

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.hostname = s.mqttHost.c_str();
    cfg.broker.address.port = s.mqttPort;
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    cfg.credentials.client_id = s.deviceName.c_str();
    if (s.mqttUser.length()) cfg.credentials.username = s.mqttUser.c_str();
    if (s.mqttPass.length()) cfg.credentials.authentication.password = s.mqttPass.c_str();
    cfg.session.keepalive = 30;
    cfg.session.last_will.topic = kStatusTopic;
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    cfg.buffer.out_size = 2048;  // discovery configs run to ~600 bytes

    g_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY, onEvent, nullptr);
    esp_mqtt_client_start(g_client);
    g_state = MqttState::Connecting;
    xTaskCreate(publisherTask, "mqttPub", 6144, nullptr, 1, nullptr);
    Serial.printf("MQTT: broker %s:%u, topics %s/<%s>/..., HA discovery %s\n", s.mqttHost.c_str(), s.mqttPort,
                  kBase, s.mqttUidTopics ? "uid" : "die type", s.haDiscovery ? "on" : "off");
}

MqttState mqttState() { return g_state; }
