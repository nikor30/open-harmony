// IR/RF gateway for the Kincony KC868-AG (ESP32), part of open-harmony.
//
// - IR receive (GPIO23) and send (GPIO2) with IRremoteESP8266: known
//   protocols are stored as protocol/value/bits, anything else as raw
//   timings with its carrier.
// - 433 MHz receive (GPIO13) and send (GPIO22) with rc-switch.
// - A library of devices, their commands and activities (macros of
//   commands with delays) in LittleFS (/db.json).
// - Web UI (web/index.html, embedded gzipped), REST API under /api and a
//   WebSocket (/ws) that streams received codes and events.
// - Home Assistant through MQTT discovery: a button per command and per
//   activity, an event entity that fires for every received IR/RF code
//   (also the codes the Harmony 900 sends), and a "last code" sensor.
//
// Settings (Wi-Fi, MQTT, name) live in NVS (Preferences "gw"); the first
// Wi-Fi comes from open_harmony_secrets.h if present, otherwise the gateway opens the
// access point "IR-Gateway-Setup" with the web UI at 192.168.4.1.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <IRac.h>
#include <RCSwitch.h>
#include <mutex>
#include "web_index.h"

// Wi-Fi and update password: root-only file outside the repository,
// /root/.config/open-harmony/open_harmony_secrets.h (include path set in platformio.ini)
#if __has_include(<open_harmony_secrets.h>)
#include <open_harmony_secrets.h>
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif
#ifndef OTA_PASS
#define OTA_PASS ""  // no password: updates open to the network
#endif
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static const int PIN_IR_RX = 23, PIN_IR_TX = 2, PIN_RF_RX = 13, PIN_RF_TX = 22;
static const char *DB_PATH = "/db.json";

// ---------------------------------------------------------------- state

struct Settings {
  String name = "IR Gateway";
  String ssid, pass;
  String mqttHost, mqttUser, mqttPass;
  uint16_t mqttPort = 1883;
} cfg;

static Preferences prefs;
static JsonDocument db;  // {"devices":[...], "activities":[...]}
static String deviceId;  // "irgw_<mac tail>"
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static WiFiClient mqttNet;
static PubSubClient mqtt(mqttNet);
static IRrecv irrecv(PIN_IR_RX, 1024, 50, true);
static IRsend irsend(PIN_IR_TX);
static RCSwitch rf;
static decode_results irResult;
static JsonDocument lastCode;  // last received code
static bool apMode;
static uint32_t lastMqttTry, rxCount, txCount;
static bool discoveryDirty = true;
static std::vector<String> publishedIds;  // discovery object ids we created

// activity runner: queued steps executed from loop()
struct Step { String device, command; uint32_t delayMs; };
static std::vector<Step> queue;
static uint32_t nextStepAt;
static String runningActivity;

// work handed from the async web task to loop(): {"op":"send"|"code"|"run"|"db", ...}
static std::vector<String> jobs;
static std::mutex jobsLock;
static volatile bool restartPending;
static uint32_t lastReconnect;

static void addJob(const String &op, const String &body) {
  std::lock_guard<std::mutex> g(jobsLock);
  jobs.push_back(op + "\n" + body);
}

// ---------------------------------------------------------------- helpers

static void logEvent(const char *type, JsonDocument &data) {
  data["type"] = type;
  String s;
  serializeJson(data, s);
  ws.textAll(s);
  Serial.println(s);
}

static String slug(const String &in) {
  String o;
  for (char c : in) {
    if (isalnum((unsigned char)c)) o += (char)tolower(c);
    else if (o.length() && o[o.length() - 1] != '_') o += '_';
  }
  while (o.endsWith("_")) o.remove(o.length() - 1);
  return o.length() ? o : String("x");
}

static void loadSettings() {
  prefs.begin("gw", true);
  cfg.name = prefs.getString("name", cfg.name);
  cfg.ssid = prefs.getString("ssid", WIFI_SSID);
  cfg.pass = prefs.getString("pass", WIFI_PASS);
  cfg.mqttHost = prefs.getString("mqttHost", "");
  cfg.mqttPort = prefs.getUShort("mqttPort", 1883);
  cfg.mqttUser = prefs.getString("mqttUser", "");
  cfg.mqttPass = prefs.getString("mqttPass", "");
  prefs.end();
}

static void saveSettings() {
  prefs.begin("gw", false);
  prefs.putString("name", cfg.name);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("mqttHost", cfg.mqttHost);
  prefs.putUShort("mqttPort", cfg.mqttPort);
  prefs.putString("mqttUser", cfg.mqttUser);
  prefs.putString("mqttPass", cfg.mqttPass);
  prefs.end();
}

static void loadDb() {
  File f = LittleFS.open(DB_PATH, "r");
  if (!f || deserializeJson(db, f)) {
    db.clear();
    db["devices"].to<JsonArray>();
    db["activities"].to<JsonArray>();
  }
  if (f) f.close();
}

static bool saveDb() {
  File f = LittleFS.open("/db.tmp", "w");
  if (!f) return false;
  serializeJson(db, f);
  f.close();
  LittleFS.remove(DB_PATH);
  LittleFS.rename("/db.tmp", DB_PATH);
  discoveryDirty = true;
  return true;
}

static JsonObject findDevice(const String &id) {
  for (JsonObject d : db["devices"].as<JsonArray>())
    if (d["id"] == id) return d;
  return JsonObject();
}

static JsonObject findCommand(const String &dev, const String &cmd) {
  JsonObject d = findDevice(dev);
  if (d.isNull()) return JsonObject();
  for (JsonObject c : d["commands"].as<JsonArray>())
    if (c["id"] == cmd) return c;
  return JsonObject();
}

static JsonObject findActivity(const String &id) {
  for (JsonObject a : db["activities"].as<JsonArray>())
    if (a["id"] == id) return a;
  return JsonObject();
}

// ---------------------------------------------------------------- IR / RF

// send one code object {"kind":"ir"|"rf", ...}; returns false if malformed
static bool sendCode(JsonVariantConst code) {
  String kind = code["kind"] | "";
  if (kind == "ir") {
    String proto = code["protocol"] | "UNKNOWN";
    decode_type_t t = strToDecodeType(proto.c_str());
    irrecv.disableIRIn();  // do not hear ourselves
    bool ok = false;
    if (t != decode_type_t::UNKNOWN && t != decode_type_t::RAW) {
      uint64_t v = strtoull((code["value"] | "0"), nullptr, 16);
      uint16_t bits = code["bits"] | 0;
      uint16_t repeat = code["repeat"] | 0;
      if (hasACState(t)) {
        // state-based protocols (air conditioners): hex string of the state bytes
        String hex = code["state"] | "";
        uint8_t state[kStateSizeMax];
        size_t n = hex.length() / 2;
        for (size_t i = 0; i < n && i < kStateSizeMax; i++)
          state[i] = strtoul(hex.substring(2 * i, 2 * i + 2).c_str(), nullptr, 16);
        ok = irsend.send(t, state, n);
      } else {
        ok = irsend.send(t, v, bits, repeat);
      }
    } else if (code["raw"].is<JsonArrayConst>()) {
      JsonArrayConst raw = code["raw"];
      std::vector<uint16_t> buf;
      for (JsonVariantConst x : raw) buf.push_back(x.as<uint16_t>());
      uint16_t khz = code["khz"] | 38;
      irsend.sendRaw(buf.data(), buf.size(), khz);
      ok = true;
    }
    irrecv.enableIRIn();
    return ok;
  }
  if (kind == "rf") {
    rf.setProtocol(code["protocol"] | 1);
    rf.setPulseLength(code["pulse"] | 350);
    rf.setRepeatTransmit(code["repeat"] | 10);
    rf.send((unsigned long)(code["code"] | 0UL), code["bits"] | 24);
    return true;
  }
  return false;
}

static bool sendCommand(const String &dev, const String &cmd) {
  JsonObject c = findCommand(dev, cmd);
  if (c.isNull()) return false;
  bool ok = sendCode(c["code"]);
  txCount++;
  JsonDocument e;
  e["device"] = dev;
  e["command"] = cmd;
  e["ok"] = ok;
  logEvent("sent", e);
  return ok;
}

static void startActivity(const String &id) {
  JsonObject a = findActivity(id);
  if (a.isNull()) return;
  queue.clear();
  for (JsonObject s : a["steps"].as<JsonArray>())
    queue.push_back({s["device"] | "", s["command"] | "", (uint32_t)(s["delay"] | 300)});
  runningActivity = id;
  nextStepAt = millis();
  JsonDocument e;
  e["activity"] = id;
  e["steps"] = (int)queue.size();
  logEvent("activity_start", e);
}

static void runQueue() {
  if (queue.empty() || (int32_t)(millis() - nextStepAt) < 0) return;
  Step s = queue.front();
  queue.erase(queue.begin());
  sendCommand(s.device, s.command);
  nextStepAt = millis() + s.delayMs;
  if (queue.empty()) {
    JsonDocument e;
    e["activity"] = runningActivity;
    logEvent("activity_done", e);
    runningActivity = "";
  }
}

static void publishReceived();

static void pollIr() {
  if (!irrecv.decode(&irResult)) return;
  JsonDocument c;
  c["kind"] = "ir";
  decode_type_t t = irResult.decode_type;
  if (irResult.repeat) {  // NEC repeat frames: not a new press
    irrecv.resume();
    return;
  }
  if (t != decode_type_t::UNKNOWN) {
    c["protocol"] = typeToString(t);
    if (hasACState(t)) {
      String hex;
      for (uint16_t i = 0; i < irResult.bits / 8; i++) {
        char b[3];
        snprintf(b, sizeof(b), "%02X", irResult.state[i]);
        hex += b;
      }
      c["state"] = hex;
    } else {
      c["value"] = uint64ToString(irResult.value, 16);
    }
    c["bits"] = irResult.bits;
  } else {
    c["protocol"] = "UNKNOWN";
    c["khz"] = 38;
    JsonArray raw = c["raw"].to<JsonArray>();
    for (uint16_t i = 1; i < irResult.rawlen; i++)
      raw.add((uint32_t)irResult.rawbuf[i] * kRawTick);
  }
  irrecv.resume();
  rxCount++;
  lastCode.set(c);
  lastCode["at"] = millis();
  JsonDocument e;
  e["code"] = c;
  logEvent("rx", e);
  publishReceived();
}

static void pollRf() {
  if (!rf.available()) return;
  JsonDocument c;
  c["kind"] = "rf";
  c["code"] = rf.getReceivedValue();
  c["bits"] = rf.getReceivedBitlength();
  c["protocol"] = rf.getReceivedProtocol();
  c["pulse"] = rf.getReceivedDelay();
  rf.resetAvailable();
  if ((c["code"] | 0UL) == 0) return;
  rxCount++;
  lastCode.set(c);
  lastCode["at"] = millis();
  JsonDocument e;
  e["code"] = c;
  logEvent("rx", e);
  publishReceived();
}

// ---------------------------------------------------------------- MQTT / HA

static String topicBase() { return "irgw/" + deviceId; }

static void deviceBlock(JsonObject dev) {
  JsonArray ids = dev["identifiers"].to<JsonArray>();
  ids.add(deviceId);
  dev["name"] = cfg.name;
  dev["manufacturer"] = "open-harmony";
  dev["model"] = "KC868-AG IR/RF gateway";
  dev["sw_version"] = FW_VERSION;
  dev["configuration_url"] = "http://" + WiFi.localIP().toString() + "/";
}

static void publishConfig(const String &component, const String &objectId, JsonDocument &doc) {
  String t = "homeassistant/" + component + "/" + deviceId + "/" + objectId + "/config";
  String s;
  serializeJson(doc, s);
  mqtt.publish(t.c_str(), s.c_str(), true);
  publishedIds.push_back(component + "/" + objectId);
}

static void publishDiscovery() {
  // remove what we published before, then publish the current library
  for (const String &id : publishedIds) {
    String t = "homeassistant/" + id.substring(0, id.indexOf('/')) + "/" + deviceId + "/" +
               id.substring(id.indexOf('/') + 1) + "/config";
    mqtt.publish(t.c_str(), "", true);
  }
  publishedIds.clear();
  String avail = topicBase() + "/status";
  for (JsonObject d : db["devices"].as<JsonArray>())
    for (JsonObject c : d["commands"].as<JsonArray>()) {
      String oid = slug(String((const char *)d["id"]) + "_" + (const char *)c["id"]);
      JsonDocument doc;
      doc["name"] = String((const char *)(d["name"] | "")) + " " + (const char *)(c["name"] | "");
      doc["unique_id"] = deviceId + "_" + oid;
      doc["command_topic"] = topicBase() + "/send/" + (const char *)d["id"] + "/" + (const char *)c["id"];
      doc["availability_topic"] = avail;
      doc["icon"] = (c["code"]["kind"] == "rf") ? "mdi:radio-tower" : "mdi:remote";
      deviceBlock(doc["device"].to<JsonObject>());
      publishConfig("button", oid, doc);
    }
  for (JsonObject a : db["activities"].as<JsonArray>()) {
    String oid = slug(String("activity_") + (const char *)a["id"]);
    JsonDocument doc;
    doc["name"] = String("Activity ") + (const char *)(a["name"] | "");
    doc["unique_id"] = deviceId + "_" + oid;
    doc["command_topic"] = topicBase() + "/activity/" + (const char *)a["id"];
    doc["availability_topic"] = avail;
    doc["icon"] = "mdi:play-box-multiple";
    deviceBlock(doc["device"].to<JsonObject>());
    publishConfig("button", oid, doc);
  }
  {  // every received code fires this event entity
    JsonDocument doc;
    doc["name"] = "Received code";
    doc["unique_id"] = deviceId + "_received";
    doc["state_topic"] = topicBase() + "/received";
    JsonArray types = doc["event_types"].to<JsonArray>();
    types.add("ir");
    types.add("rf");
    doc["availability_topic"] = avail;
    doc["icon"] = "mdi:access-point";
    deviceBlock(doc["device"].to<JsonObject>());
    publishConfig("event", "received", doc);
  }
  {
    JsonDocument doc;
    doc["name"] = "Last code";
    doc["unique_id"] = deviceId + "_last";
    doc["state_topic"] = topicBase() + "/last";
    doc["value_template"] = "{{ value_json.summary }}";
    doc["json_attributes_topic"] = topicBase() + "/last";
    doc["availability_topic"] = avail;
    doc["icon"] = "mdi:code-braces";
    deviceBlock(doc["device"].to<JsonObject>());
    publishConfig("sensor", "last", doc);
  }
  discoveryDirty = false;
}

static String codeSummary(JsonVariantConst c) {
  if (c["kind"] == "rf")
    return "RF " + String((unsigned long)(c["code"] | 0UL)) + "/" + String((int)(c["bits"] | 0));
  String p = c["protocol"] | "?";
  if (c["value"].is<const char *>()) return p + " 0x" + (const char *)c["value"];
  if (c["raw"].is<JsonArrayConst>()) return p + " raw " + String(c["raw"].size());
  return p;
}

static void publishReceived() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["event_type"] = (const char *)(lastCode["kind"] | "ir");
  doc["summary"] = codeSummary(lastCode);
  for (JsonPairConst kv : lastCode.as<JsonObjectConst>())
    if (kv.key() != "raw") doc[kv.key()] = kv.value();
  String s;
  serializeJson(doc, s);
  mqtt.publish((topicBase() + "/received").c_str(), s.c_str());
  mqtt.publish((topicBase() + "/last").c_str(), s.c_str(), true);
}

static void onMqtt(char *topic, byte *payload, unsigned int len) {
  String t(topic), base = topicBase() + "/";
  if (!t.startsWith(base)) return;
  t = t.substring(base.length());
  if (t.startsWith("send/")) {  // send/<device>/<command>
    String rest = t.substring(5);
    int k = rest.indexOf('/');
    if (k > 0) sendCommand(rest.substring(0, k), rest.substring(k + 1));
  } else if (t.startsWith("activity/")) {
    startActivity(t.substring(9));
  } else if (t == "code") {  // raw code JSON, for automations
    JsonDocument c;
    if (!deserializeJson(c, payload, len)) sendCode(c);
  }
}

static void mqttLoop() {
  if (cfg.mqttHost.isEmpty() || apMode || WiFi.status() != WL_CONNECTED) return;
  if (!mqtt.connected()) {
    if (millis() - lastMqttTry < 5000) return;
    lastMqttTry = millis();
    mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
    String avail = topicBase() + "/status";
    if (mqtt.connect(deviceId.c_str(), cfg.mqttUser.c_str(), cfg.mqttPass.c_str(), avail.c_str(), 1, true,
                     "offline")) {
      mqtt.publish(avail.c_str(), "online", true);
      mqtt.subscribe((topicBase() + "/send/+/+").c_str());
      mqtt.subscribe((topicBase() + "/activity/+").c_str());
      mqtt.subscribe((topicBase() + "/code").c_str());
      discoveryDirty = true;
    }
    return;
  }
  if (discoveryDirty) publishDiscovery();
  mqtt.loop();
}

// ---------------------------------------------------------------- web

static void sendJson(AsyncWebServerRequest *req, JsonDocument &doc, int code = 200) {
  String s;
  serializeJson(doc, s);
  req->send(code, "application/json", s);
}

static void statusJson(JsonDocument &d) {
  d["name"] = cfg.name;
  d["id"] = deviceId;
  d["fw"] = FW_VERSION;
  d["ip"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  d["ap"] = apMode;
  d["ssid"] = cfg.ssid;
  d["rssi"] = WiFi.RSSI();
  d["uptime"] = millis() / 1000;
  d["mqtt"] = mqtt.connected();
  d["mqttHost"] = cfg.mqttHost;
  d["mqttPort"] = cfg.mqttPort;
  d["mqttUser"] = cfg.mqttUser;
  d["rx"] = rxCount;
  d["tx"] = txCount;
  d["heap"] = ESP.getFreeHeap();
  d["fsUsed"] = LittleFS.usedBytes();
  d["fsTotal"] = LittleFS.totalBytes();
  d["running"] = runningActivity;
}

// collect a request body (may arrive in chunks), then call done(body)
static void onBody(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) req->_tempObject = new String();
  String *s = (String *)req->_tempObject;
  s->concat((const char *)data, len);
}

static String takeBody(AsyncWebServerRequest *req) {
  String *s = (String *)req->_tempObject;
  String b = s ? *s : String();
  delete s;
  req->_tempObject = nullptr;
  return b;
}

static void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    AsyncWebServerResponse *r = req->beginResponse(200, "text/html", WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
    r->addHeader("Content-Encoding", "gzip");
    req->send(r);
  });
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument d;
    statusJson(d);
    sendJson(req, d);
  });
  server.on("/api/db", HTTP_GET, [](AsyncWebServerRequest *req) { sendJson(req, db); });
  server.on("/api/last", HTTP_GET, [](AsyncWebServerRequest *req) { sendJson(req, lastCode); });
  server.on(
      "/api/db", HTTP_PUT,
      [](AsyncWebServerRequest *req) {
        String body = takeBody(req);
        JsonDocument d;
        if (deserializeJson(d, body) || !d["devices"].is<JsonArray>() || !d["activities"].is<JsonArray>()) {
          req->send(400, "application/json", "{\"error\":\"bad db\"}");
          return;
        }
        addJob("db", body);
        req->send(200, "application/json", "{}");
      },
      nullptr, onBody);
  server.on(
      "/api/send", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        addJob("send", takeBody(req));
        req->send(202, "application/json", "{}");
      },
      nullptr, onBody);
  server.on(
      "/api/run", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        addJob("run", takeBody(req));
        req->send(202, "application/json", "{}");
      },
      nullptr, onBody);
  server.on(
      "/api/config", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        JsonDocument d;
        if (deserializeJson(d, takeBody(req))) {
          req->send(400);
          return;
        }
        bool wifi = false;
        if (d["name"].is<const char *>()) cfg.name = (const char *)d["name"];
        if (d["ssid"].is<const char *>()) { cfg.ssid = (const char *)d["ssid"]; wifi = true; }
        if (d["pass"].is<const char *>()) { cfg.pass = (const char *)d["pass"]; wifi = true; }
        if (d["mqttHost"].is<const char *>()) cfg.mqttHost = (const char *)d["mqttHost"];
        if (d["mqttPort"].is<int>()) cfg.mqttPort = d["mqttPort"];
        if (d["mqttUser"].is<const char *>()) cfg.mqttUser = (const char *)d["mqttUser"];
        if (d["mqttPass"].is<const char *>()) cfg.mqttPass = (const char *)d["mqttPass"];
        saveSettings();
        mqtt.disconnect();
        lastMqttTry = 0;
        discoveryDirty = true;
        if (wifi) restartPending = true;
        req->send(200, "application/json", "{}");
      },
      nullptr, onBody);
  // firmware update over the network: POST the .bin as multipart "firmware",
  // with the update password in the header X-Update-Password
  server.on(
      "/api/update", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        if (req->_tempObject) {  // refused in the upload handler
          req->_tempObject = nullptr;
          req->send(403, "application/json", "{\"ok\":false,\"error\":\"password\"}");
          return;
        }
        bool ok = !Update.hasError();
        req->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        if (ok) restartPending = true;
      },
      [](AsyncWebServerRequest *req, const String &, size_t index, uint8_t *data, size_t len, bool final) {
        if (index == 0) {
          const AsyncWebHeader *h = req->getHeader("X-Update-Password");
          if (strlen(OTA_PASS) && (!h || h->value() != OTA_PASS)) {
            req->_tempObject = (void *)1;
            return;
          }
          Update.begin(UPDATE_SIZE_UNKNOWN);
        }
        if (req->_tempObject) return;
        if (len) Update.write(data, len);
        if (final) Update.end(true);
      });
  ws.onEvent([](AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *, uint8_t *, size_t) {
    if (type == WS_EVT_CONNECT) {
      JsonDocument d;
      statusJson(d);
      d["type"] = "status";
      String s;
      serializeJson(d, s);
      client->text(s);
    }
  });
  server.addHandler(&ws);
  server.onNotFound([](AsyncWebServerRequest *req) { req->send(404, "text/plain", "not found"); });
  server.begin();
}

// ---------------------------------------------------------------- setup/loop

static void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(slug(cfg.name).c_str());
  if (cfg.ssid.length()) {
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {  // set-up access point with the web UI
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("IR-Gateway-Setup");
  }
  ArduinoOTA.setHostname("ir-gateway");  // also announces itself over mDNS
  if (strlen(OTA_PASS)) ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() { irrecv.disableIRIn(); });
  ArduinoOTA.begin();
  MDNS.addService("http", "tcp", 80);
}

void setup() {
  Serial.begin(115200);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char id[16];
  snprintf(id, sizeof(id), "irgw_%02x%02x%02x", mac[3], mac[4], mac[5]);
  deviceId = id;
  LittleFS.begin(true, "/littlefs", 10, "littlefs");  // our partition label (default is "spiffs")
  loadSettings();
  loadDb();
  irsend.begin();
  irrecv.setUnknownThreshold(12);
  irrecv.enableIRIn();
  rf.enableReceive(digitalPinToInterrupt(PIN_RF_RX));
  rf.enableTransmit(PIN_RF_TX);
  startWifi();
  mqtt.setBufferSize(1536);
  mqtt.setCallback(onMqtt);
  setupWeb();
  Serial.printf("open-harmony IR gateway %s, %s, %s\n", FW_VERSION, deviceId.c_str(),
                (apMode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str());
}

void loop() {
  pollIr();
  pollRf();
  for (;;) {
    String job;
    {
      std::lock_guard<std::mutex> g(jobsLock);
      if (jobs.empty()) break;
      job = jobs.front();
      jobs.erase(jobs.begin());
    }
    int nl = job.indexOf('\n');
    String op = job.substring(0, nl);
    JsonDocument d;
    if (deserializeJson(d, job.substring(nl + 1))) continue;
    if (op == "db") {
      db.set(d);
      saveDb();
      JsonDocument e;
      logEvent("db", e);
    } else if (op == "run") {
      startActivity(d["activity"] | "");
    } else if (d["device"].is<const char *>()) {
      sendCommand(d["device"] | "", d["command"] | "");
    } else {
      bool ok = sendCode(d["code"]);
      txCount++;
      JsonDocument e;
      e["code"] = d["code"];
      e["ok"] = ok;
      logEvent("sent", e);
    }
  }
  runQueue();
  mqttLoop();
  ArduinoOTA.handle();
  ws.cleanupClients();
  if (restartPending) {
    delay(300);
    ESP.restart();
  }
  if (!apMode && WiFi.status() != WL_CONNECTED && millis() - lastReconnect > 10000) {
    lastReconnect = millis();
    WiFi.reconnect();
  }
  delay(2);
}
