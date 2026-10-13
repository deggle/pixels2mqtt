#include "web.h"

#include <ESPAsyncWebServer.h>

#include "dice.h"
#include "mqtt.h"
#include "net.h"

namespace {

AsyncWebServer g_server(80);
AsyncWebSocket g_ws("/ws");
constexpr uint32_t kPushIntervalMs = 100;  // max update rate to browsers
Settings*      g_settings = nullptr;
uint32_t       g_rebootAtMs = 0;  // 0 = no reboot pending

const char kStyle[] PROGMEM = R"css(
:root{color-scheme:light dark;--bg:#f6f6f4;--fg:#1d1d1b;--muted:#6b6b66;--card:#fff;--line:#d9d9d4;--accent:#2f6fde}
@media (prefers-color-scheme:dark){:root{--bg:#161615;--fg:#ececea;--muted:#9a9a94;--card:#21211f;--line:#3a3a37;--accent:#6e9cf0}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,sans-serif}
main{max-width:480px;margin:0 auto;padding:16px}h1{font-size:1.3rem;margin:.5rem 0 1rem}
fieldset{border:1px solid var(--line);border-radius:8px;background:var(--card);margin:0 0 16px;padding:12px 14px}
legend{font-weight:600;padding:0 6px}label{display:block;margin:10px 0 4px;font-size:.9rem;color:var(--muted)}
input{width:100%;padding:9px 10px;font:inherit;color:var(--fg);background:var(--bg);border:1px solid var(--line);border-radius:6px}
button,.btn{display:inline-block;padding:10px 18px;font:inherit;color:#fff;background:var(--accent);border:0;border-radius:6px;text-decoration:none}
.note{color:var(--muted);font-size:.85rem}
label.chk{display:flex;align-items:center;gap:8px;color:var(--fg);font-size:1rem}label.chk input{width:auto}.err{color:#d33}dl{display:grid;grid-template-columns:auto 1fr;gap:6px 14px}dt{color:var(--muted)}dd{margin:0}
)css";

String htmlEscape(const String& in) {
    String out;
    out.reserve(in.length());
    for (char c : in) {
        switch (c) {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += c;
        }
    }
    return out;
}

// Hostname-safe: lowercase letters, digits and '-', max 32, no leading/trailing '-'
String sanitiseDeviceName(const String& in) {
    String out;
    for (char c : in) {
        if (out.length() >= 32) break;
        c = tolower(c);
        if (isalnum(c)) out += c;
        else if (out.length() && !out.endsWith("-")) out += '-';
    }
    while (out.endsWith("-")) out.remove(out.length() - 1);
    return out.length() ? out : String("pixels2mqtt");
}

String pageStart(const char* title) {
    String h;
    h.reserve(4096);
    h += F("<!doctype html><html lang=en><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'><title>");
    h += title;
    h += F("</title><style>");
    h += FPSTR(kStyle);
    h += F("</style></head><body><main>");
    return h;
}

String field(const char* label, const char* name, const String& value,
             const char* type = "text", const char* extra = "") {
    String h;
    h += F("<label for=");  h += name; h += '>'; h += label; h += F("</label>");
    h += F("<input id=");   h += name;
    h += F(" name=");       h += name;
    h += F(" type=");       h += type;
    h += F(" value=\"");    h += htmlEscape(value); h += '"';
    h += ' '; h += extra;   h += '>';
    return h;
}

String checkbox(const char* label, const char* name, bool checked) {
    String h = F("<label class=chk><input type=checkbox value=1 name=");
    h += name;
    if (checked) h += F(" checked");
    h += '>'; h += label; h += F("</label>");
    return h;
}

String settingsPage(const String& error = "") {
    const Settings& s = *g_settings;
    String h = pageStart("pixels2mqtt settings");
    h += F("<h1>pixels2mqtt settings</h1>");
    if (error.length()) { h += F("<p class=err>"); h += htmlEscape(error); h += F("</p>"); }
    h += F("<form method=post action=/settings>");

    h += F("<fieldset><legend>Wi-Fi</legend>");
    h += field("Network (SSID)", "ssid", s.wifiSsid, "text", "list=nets required maxlength=32 autocomplete=off");
    h += F("<datalist id=nets>");
    for (const auto& n : netScanResults()) { h += F("<option value=\""); h += htmlEscape(n); h += F("\">"); }
    h += F("</datalist>");
    // Never echo stored passwords back into the page; blank means "keep".
    h += field("Password", "wifiPass", "", "password",
               s.wifiPass.length() ? "maxlength=63 placeholder='unchanged'" : "maxlength=63");
    h += F("</fieldset>");

    h += F("<fieldset><legend>Device</legend>");
    h += field("Name", "devName", s.deviceName, "text", "maxlength=32");
    h += F("<p class=note>Used as the hostname: <i>name</i>.local</p></fieldset>");

    h += F("<fieldset><legend>MQTT</legend>");
    h += field("Broker host", "mqttHost", s.mqttHost, "text", "placeholder='blank = disabled'");
    h += field("Port", "mqttPort", String(s.mqttPort), "number", "min=1 max=65535");
    h += field("Username", "mqttUser", s.mqttUser);
    h += field("Password", "mqttPass", "", "password",
               s.mqttPass.length() ? "placeholder='unchanged'" : "");
    h += checkbox("Use UID in topics", "mqttUid", s.mqttUidTopics);
    h += F("<p class=note>Off: pixels/d20/rolled (one set). On: pixels/3D3A8B73/rolled, for several dice of the same type.</p>");
    h += checkbox("Home Assistant discovery", "haDisc", s.haDiscovery);
    h += F("<p class=note>Each die appears in HA as a device with roll, rolling and battery entities.</p></fieldset>");

    h += F("<button type=submit>Save and reboot</button></form></main></body></html>");
    return h;
}

// Live dice view. Static page; all data arrives over /ws as JSON.
const char kDashboard[] PROGMEM = R"html(
<header><h1 id=title>pixels2mqtt</h1><span id=mqtt class=pill hidden>MQTT</span><span id=conn class=pill>connecting</span><a href=/settings>Settings</a></header>
<section id=total hidden><div class=tl></div><div class=tv></div><div class=tb></div></section>
<section id=grid></section>
<p id=empty class=note>No dice yet. Pick one up or roll it to wake it.</p>
<style>
main{max-width:960px}header{display:flex;align-items:center;gap:12px;margin-bottom:12px}header h1{margin:0;flex:1}
header a{color:var(--accent)}.pill{font-size:.75rem;padding:2px 8px;border-radius:99px;border:1px solid var(--line);color:var(--muted)}
.pill.ok{color:#2a9d4b;border-color:#2a9d4b}
#grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px}
.die{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px;position:relative}
.die.off{opacity:.45}.die h2{font-size:.95rem;margin:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.die .id{font-size:.75rem;color:var(--muted);font-family:ui-monospace,monospace}
.die .val{font-size:3.2rem;font-weight:700;text-align:center;line-height:1.1;margin:10px 0 4px;font-variant-numeric:tabular-nums}
.die .st{text-align:center;font-size:.85rem;color:var(--muted);min-height:1.2em}
.die.rolling{border-color:#3b6fe0}.die.rolling .val{color:var(--muted);animation:pulse .6s ease-in-out infinite alternate}
.die.rolling .st{color:#3b6fe0}.die.crooked .st{color:#d33}
.die.flash{animation:flash 1.2s ease-out}
@keyframes pulse{to{opacity:.35}}@keyframes flash{0%{box-shadow:0 0 0 3px #2a9d4b;border-color:#2a9d4b}100%{box-shadow:0 0 0 0 transparent}}
.bat{display:flex;align-items:center;gap:6px;font-size:.75rem;color:var(--muted);margin-top:8px}
.bar{flex:1;height:5px;background:var(--line);border-radius:3px;overflow:hidden}.bar i{display:block;height:100%;background:#2a9d4b}
.bar i.low{background:#d33}
#total{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 16px;margin-bottom:12px;
 display:grid;grid-template-columns:auto 1fr;align-items:center;gap:2px 16px}
#total .tl{grid-column:2;font-size:.85rem;color:var(--muted)}#total .tb{grid-column:2;font-size:.9rem;font-variant-numeric:tabular-nums}
#total .tv{grid-row:1/3;font-size:2.6rem;font-weight:700;font-variant-numeric:tabular-nums;min-width:2ch}
#total.rolling{border-color:#3b6fe0}#total.rolling .tv{color:var(--muted);animation:pulse .6s ease-in-out infinite alternate}
#total.flash{animation:flash 1.2s ease-out}
</style>
<script>
const STATES=['','Rolled','Handling','Rolling…','Crooked','On face'];
const grid=document.getElementById('grid'),empty=document.getElementById('empty'),conn=document.getElementById('conn'),mq=document.getElementById('mqtt');
const seen={};  // slot -> last rollCount, to flash only on new rolls
const ORDER=['d4','d6','d8','d10','d00','d12','d20'];
function card(d){
  let el=document.getElementById('die'+d.slot);
  if(!el){
    el=document.createElement('div');el.id='die'+d.slot;el.className='die';
    el.innerHTML='<h2></h2><div class=id></div><div class=val></div><div class=st></div>'+
      '<div class=bat><div class=bar><i></i></div><span></span></div>';
    grid.appendChild(el);
  }
  return el;
}
const tot=document.getElementById('total');let totSeq;
function renderTotal(g){
  if(!g){tot.hidden=true;return}
  tot.hidden=false;
  const parts=g.parts.slice().sort((a,b)=>ORDER.indexOf(a[0])-ORDER.indexOf(b[0]));
  tot.querySelector('.tv').textContent=g.total;
  tot.querySelector('.tl').textContent=g.active?'Rolling… '+g.landed+' of '+g.dice+' landed':
    'Last roll total · '+g.dice+(g.dice===1?' die':' dice')+(g.landed<g.dice?' ('+(g.dice-g.landed)+' without a result, not counted)':'');
  tot.querySelector('.tb').textContent=parts.map(p=>p[0]+' '+(p[1]??'…')).join('  +  ');
  tot.classList.toggle('rolling',g.active);
  if(!g.active&&totSeq!==undefined&&g.seq!==totSeq){tot.classList.remove('flash');void tot.offsetWidth;tot.classList.add('flash')}
  if(!g.active)totSeq=g.seq;
}
function render(m){
  mq.hidden=m.mqtt==='off';mq.className='pill'+(m.mqtt==='connected'?' ok':'');mq.title='MQTT '+m.mqtt;
  renderTotal(m.group);
  if(m.device)document.getElementById('title').textContent=m.device;
  empty.hidden=m.dice.length>0;
  for(const d of m.dice){
    const el=card(d),rolling=d.state===3;
    el.querySelector('h2').textContent=d.type||'Pixel';
    el.querySelector('.id').textContent=d.id||d.addr;
    el.style.order=ORDER.indexOf(d.type)+1;  // standard set order; unknown types first
    // rolled / rolling / on face: the face it's showing now; handling / crooked: last result
    const live=d.state===1||d.state===3||d.state===5;
    el.querySelector('.val').textContent=live?d.value:(d.last||'–');
    el.querySelector('.st').textContent=d.online?(STATES[d.state]||''):'Asleep';
    const bar=el.querySelector('.bar i');bar.style.width=d.batt+'%';bar.className=d.batt<20?'low':'';
    el.querySelector('.bat span').textContent=d.batt+'%'+(d.charging?' charging':'')+' · '+d.rssi+' dBm';
    el.classList.toggle('off',!d.online);
    el.classList.toggle('rolling',rolling&&d.online);
    el.classList.toggle('crooked',d.state===4);
    if(seen[d.slot]!==undefined&&d.rolls>seen[d.slot]){
      el.classList.remove('flash');void el.offsetWidth;el.classList.add('flash');
    }
    seen[d.slot]=d.rolls;
  }
}
function connect(){
  const ws=new WebSocket('ws://'+location.host+'/ws');
  ws.onopen=()=>{conn.textContent='live';conn.className='pill ok'};
  ws.onmessage=e=>render(JSON.parse(e.data));
  ws.onclose=()=>{conn.textContent='reconnecting';conn.className='pill';setTimeout(connect,2000)};
}
connect();
</script>
)html";

String dashboardPage() {
    String h = pageStart("pixels2mqtt");
    h += FPSTR(kDashboard);
    h += F("</main></body></html>");
    return h;
}

void jsonString(String& out, const char* s) {
    out += '"';
    for (; *s; ++s) {
        const char c = *s;
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((uint8_t)c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
        else out += c;
    }
    out += '"';
}

String diceJson() {
    DieState dice[kMaxDice];
    diceSnapshot(dice);
    String j;
    j.reserve(2048);
    j += F("{\"device\":");
    jsonString(j, g_settings->deviceName.c_str());
    const MqttState ms = mqttState();
    j += F(",\"mqtt\":\"");
    j += ms == MqttState::Connected ? F("connected") : ms == MqttState::Connecting ? F("connecting") : F("off");
    j += '"';
    j += F(",\"dice\":[");
    bool first = true;
    for (size_t i = 0; i < kMaxDice; ++i) {
        const DieState& d = dice[i];
        if (!d.used) continue;
        if (!first) j += ',';
        first = false;
        char id[9] = "";
        if (d.pixelId) snprintf(id, sizeof(id), "%08lX", (unsigned long)d.pixelId);
        // value/last are what the die shows (d00 reads x10), computed here so
        // every consumer (web, later MQTT) agrees
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"slot\":%u,\"online\":%s,\"id\":\"%s\",\"type\":\"%s\",\"state\":%u,"
                 "\"value\":%u,\"last\":%u,\"rolls\":%lu,\"batt\":%u,\"charging\":%s,\"rssi\":%d,"
                 "\"addr\":\"%s\",\"name\":",
                 (unsigned)i, d.online ? "true" : "false", id, dieLabel(d.dieType, d.ledCount).c_str(),
                 d.rollState, dieFaceValue(d.dieType, d.face),
                 d.rollCount ? dieFaceValue(d.dieType, d.lastRollFace) : 0,
                 (unsigned long)d.rollCount, d.battery, d.charging ? "true" : "false", d.rssi, d.address);
        j += buf;
        jsonString(j, d.name);
        j += '}';
    }
    j += ']';

    RollGroup g;
    diceRollGroup(g);
    if (g.active || g.seq) {
        uint8_t landed, count;
        const uint16_t total = rollGroupTotal(g, &landed, &count);
        char buf[96];
        snprintf(buf, sizeof(buf),
                 ",\"group\":{\"active\":%s,\"seq\":%lu,\"total\":%u,\"landed\":%u,\"dice\":%u,\"parts\":[",
                 g.active ? "true" : "false", (unsigned long)g.seq, total, landed, count);
        j += buf;
        bool firstPart = true;
        for (size_t i = 0; i < kMaxDice; ++i) {
            if (!(g.members & (1u << i))) continue;
            if (!firstPart) j += ',';
            firstPart = false;
            j += F("[\"");
            j += dieLabel(dice[i].dieType, dice[i].ledCount);
            j += F("\",");
            j += g.value[i] ? String(g.value[i]) : String("null");
            j += ']';
        }
        j += F("]}");
    }
    j += '}';
    return j;
}

String paramOr(AsyncWebServerRequest* req, const char* name, const String& fallback) {
    const AsyncWebParameter* p = req->getParam(name, true);
    return p ? p->value() : fallback;
}

void handleSave(AsyncWebServerRequest* req) {
    Settings next = *g_settings;
    next.wifiSsid = paramOr(req, "ssid", "");
    next.wifiSsid.trim();
    if (next.wifiSsid.isEmpty() || next.wifiSsid.length() > 32) {
        req->send(400, "text/html", settingsPage("Enter a Wi-Fi network name (up to 32 characters)."));
        return;
    }
    const String wifiPass = paramOr(req, "wifiPass", "");
    if (wifiPass.length()) next.wifiPass = wifiPass;
    if (next.wifiPass.length() && (next.wifiPass.length() < 8 || next.wifiPass.length() > 63)) {
        req->send(400, "text/html", settingsPage("Wi-Fi password must be 8 to 63 characters."));
        return;
    }
    next.deviceName = sanitiseDeviceName(paramOr(req, "devName", next.deviceName));
    next.mqttHost = paramOr(req, "mqttHost", "");
    next.mqttHost.trim();
    const long port = paramOr(req, "mqttPort", "1883").toInt();
    next.mqttPort = (port >= 1 && port <= 65535) ? port : 1883;
    next.mqttUser = paramOr(req, "mqttUser", "");
    const String mqttPass = paramOr(req, "mqttPass", "");
    if (mqttPass.length()) next.mqttPass = mqttPass;
    next.mqttUidTopics = req->hasParam("mqttUid", true);  // unticked boxes aren't sent
    next.haDiscovery = req->hasParam("haDisc", true);

    *g_settings = next;
    settingsSave(next);
    Serial.println("Settings saved, rebooting");

    String h = pageStart("Saved");
    h += F("<h1>Saved</h1><p>Rebooting and joining <b>");
    h += htmlEscape(next.wifiSsid);
    h += F("</b>.</p><p>Reconnect your phone or computer to that network, then open <b>http://");
    h += htmlEscape(next.deviceName);
    h += F(".local/</b>. If it doesn't appear within a minute, the device will bring the "
           "<b>pixels2mqtt-setup</b> network back.</p></main></body></html>");
    req->send(200, "text/html", h);
    g_rebootAtMs = millis() + 1500;  // let the response go out first
}

}  // namespace

void webBegin(Settings& s) {
    g_settings = &s;

    g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", netMode() == NetMode::SetupAp ? settingsPage() : dashboardPage());
    });

    // New browser: send it the full table straight away
    g_ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type,
                    void*, uint8_t*, size_t) {
        if (type == WS_EVT_CONNECT) {
            client->client()->setNoDelay(true);  // small frames: don't wait on Nagle
            client->text(diceJson());
        }
    });
    g_server.addHandler(&g_ws);
    g_server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", settingsPage());
    });
    g_server.on("/settings", HTTP_POST, handleSave);

    // In setup mode every unknown URL (including the OS captive-portal probes
    // such as /generate_204 and /hotspot-detect.html) redirects to the form,
    // which makes phones pop up the sign-in page.
    g_server.onNotFound([](AsyncWebServerRequest* req) {
        if (netMode() == NetMode::SetupAp) {
            req->redirect(String("http://") + netIp() + "/");
        } else {
            req->send(404, "text/plain", "Not found");
        }
    });

    g_server.begin();
}

void webLoop() {
    if (g_rebootAtMs && (int32_t)(millis() - g_rebootAtMs) >= 0) {
        ESP.restart();
    }

    // Push the dice table when it changes, rate-limited
    static uint32_t sentVersion = 0, lastPushMs = 0, lastCleanupMs = 0;
    const uint32_t now = millis();
    static MqttState sentMqtt = MqttState::Off;
    if ((diceVersion() != sentVersion || mqttState() != sentMqtt) && now - lastPushMs >= kPushIntervalMs) {
        sentVersion = diceVersion();
        sentMqtt = mqttState();
        lastPushMs = now;
        if (g_ws.count()) g_ws.textAll(diceJson());
    }
    if (now - lastCleanupMs >= 1000) {
        lastCleanupMs = now;
        g_ws.cleanupClients();  // drop dead connections, cap client count
    }
}
