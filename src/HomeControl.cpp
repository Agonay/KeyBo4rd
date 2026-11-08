#include "HomeControl.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "MenuManager.h"

// Plain HTTP to Home Assistant on the LAN. HTTPS is not an option while BLE is
// up: ~25 KB are left after Wi-Fi init and a TLS handshake needs more (see the
// OTA notes in AGENTS.md). Wi-Fi starts on first use and stops after
// WIFI_IDLE_OFF_MS, so the radio is only shared with BLE while Home mode is used.

static const char *NS = "kbd";
static const unsigned long HOME_MODE_TIMEOUT_MS = 60UL * 1000UL;
static const unsigned long WIFI_IDLE_OFF_MS = 5UL * 60UL * 1000UL;
static const unsigned long WIFI_CONNECT_MS = 10000;

struct Slot {
  char name[17];
  char press[48];   // entity toggled by the knob press
  char turn[48];    // entity adjusted by rotation (same as press if only one given)
};

static Slot slots[HOME_MAX_SLOTS];
static int slotCount = 0;
static String haUrl, haToken, wifiSsid, wifiPass;

// ---------- state shared between loop() and the worker task ----------
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static int reqSlot = 0;
static int reqPresses = 0;
static int reqTurn = 0;
static bool reqRefresh = false;
static char status[22] = "";
static volatile uint32_t statusVer = 0;
static volatile unsigned long lastUseMs = 0;   // last Home action, drives Wi-Fi idle-off

static TaskHandle_t worker = nullptr;
static bool modeActive = false;
static int activeSlot = 0;
static unsigned long lastInputMs = 0;
static volatile uint32_t panelVer = 0;

static void setStatus(const char *s) {
  portENTER_CRITICAL(&mux);
  if (strncmp(status, s, sizeof(status)) != 0) {
    strlcpy(status, s, sizeof(status));
    ++statusVer;
  }
  portEXIT_CRITICAL(&mux);
}

// ---------- config ----------
static void trimCopy(char *dst, size_t len, String s) {
  s.trim();
  strlcpy(dst, s.c_str(), len);
}

// One device per line: "Name = entity[, turn_entity]". Line N is Fn+N.
static void parseSlots(const String &text) {
  slotCount = 0;
  int start = 0;
  while (start <= (int)text.length() && slotCount < HOME_MAX_SLOTS) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = text.length();
    String line = text.substring(start, end);
    start = end + 1;
    line.trim();
    const int eq = line.indexOf('=');
    if (!line.length() || line[0] == '#' || eq < 0) continue;
    Slot &s = slots[slotCount];
    trimCopy(s.name, sizeof(s.name), line.substring(0, eq));
    String ents = line.substring(eq + 1);
    const int comma = ents.indexOf(',');
    trimCopy(s.press, sizeof(s.press), comma < 0 ? ents : ents.substring(0, comma));
    trimCopy(s.turn, sizeof(s.turn), comma < 0 ? ents : ents.substring(comma + 1));
    if (!strchr(s.press, '.')) continue;   // not an entity id
    if (!strchr(s.turn, '.')) strlcpy(s.turn, s.press, sizeof(s.turn));
    ++slotCount;
  }
}

static String normalizeUrl(String u) {
  u.trim();
  while (u.endsWith("/")) u.remove(u.length() - 1);
  if (u.length() && !u.startsWith("http://") && !u.startsWith("https://")) u = "http://" + u;
  return u;
}

void homeInit() {
  Preferences p;
  p.begin(NS, true);
  haUrl = normalizeUrl(p.getString("haUrl", ""));
  haToken = p.getString("haToken", "");
  const String text = p.getString("haSlots", "");
  wifiSsid = p.getString("wifiSsid", "");
  wifiPass = p.getString("wifiPass", "");
  p.end();
  parseSlots(text);
  Serial.printf("[HOME] %d device(s), HA %s, token %s\n", slotCount,
                haUrl.length() ? haUrl.c_str() : "not set", haToken.length() ? "set" : "missing");
}

bool homeConfigured() { return haUrl.length() && haToken.length() && slotCount > 0 && wifiSsid.length(); }
String homeUrl() { Preferences p; p.begin(NS, true); String s = p.getString("haUrl", ""); p.end(); return s; }
String homeSlotsText() { Preferences p; p.begin(NS, true); String s = p.getString("haSlots", ""); p.end(); return s; }
bool homeHasToken() { Preferences p; p.begin(NS, true); bool t = p.getString("haToken", "").length() > 0; p.end(); return t; }

void homeSaveConfig(const String &url, const String &token, const String &slotsText) {
  Preferences p;
  p.begin(NS, false);
  p.putString("haUrl", normalizeUrl(url));
  if (token.length()) p.putString("haToken", token);
  p.putString("haSlots", slotsText);
  p.end();
}

// ---------- Wi-Fi ----------
static bool wifiStarted = false;

static bool ensureWifi() {
  if (wifiStarted && WiFi.status() == WL_CONNECTED) return true;
  if (!wifiStarted) {
    setStatus("Wi-Fi...");
    Serial.printf("[HOME] Wi-Fi start, heap %u\n", ESP.getFreeHeap());
    if (!WiFi.mode(WIFI_STA)) { setStatus("Wi-Fi: no memory"); return false; }
    WiFi.setSleep(true);   // modem sleep is required for Wi-Fi + BLE coexistence
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    wifiStarted = true;
  }
  const unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_MS) vTaskDelay(pdMS_TO_TICKS(100));
  if (WiFi.status() != WL_CONNECTED) { setStatus("No Wi-Fi"); return false; }
  Serial.printf("[HOME] Wi-Fi up, IP %s, heap %u\n", WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
  return true;
}

static void wifiStop() {
  if (!wifiStarted) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiStarted = false;
  Serial.printf("[HOME] Wi-Fi off (idle), heap %u\n", ESP.getFreeHeap());
}

// ---------- HTTP ----------
static const char *httpError(int code) {
  switch (code) {
    case 401: return "HA: bad token";
    case 404: return "HA: unknown entity";
    case 400: return "HA: bad request";
    default:  return code < 0 ? "HA unreachable" : "HA error";
  }
}

static int haRequest(const char *method, const String &path, const String &body, String *resp) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  if (!http.begin(client, haUrl + path)) return -1;
  http.addHeader("Authorization", "Bearer " + haToken);
  int code;
  if (!strcmp(method, "POST")) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
  } else {
    code = http.GET();
  }
  if (resp && code == 200) {
    const int size = http.getSize();
    if (size < 0 || size <= 8192) *resp = http.getString();   // skip huge states (heap is tight)
  }
  http.end();
  if (code != 200) Serial.printf("[HOME] %s %s -> %d\n", method, path.c_str(), code);
  return code;
}

static String domainOf(const char *entity) {
  const char *dot = strchr(entity, '.');
  return dot ? String(entity).substring(0, dot - entity) : String();
}

static int callService(const String &service, const char *entity, const String &extra = "") {
  String body = String("{\"entity_id\":\"") + entity + "\"" + extra + "}";
  return haRequest("POST", "/api/services/" + service, body, nullptr);
}

static int doPress(const Slot &s) {
  const String d = domainOf(s.press);
  if (d == "scene" || d == "script") return callService(d + "/turn_on", s.press);
  if (d == "button" || d == "input_button") return callService(d + "/press", s.press);
  if (d == "media_player") return callService("media_player/media_play_pause", s.press);
  return callService("homeassistant/toggle", s.press);
}

// Returns the last HTTP code, 0 when the entity has no dial action.
static int doTurn(const Slot &s, int steps) {
  const String d = domainOf(s.turn);
  const int n = min(abs(steps), 5);
  const bool up = steps > 0;
  if (d == "light")
    return callService("light/turn_on", s.turn, ",\"brightness_step_pct\":" + String(constrain(steps * 10, -100, 100)));
  if (d == "cover") return callService(up ? "cover/open_cover" : "cover/close_cover", s.turn);
  String svc;
  if (d == "fan") svc = up ? "fan/increase_speed" : "fan/decrease_speed";
  else if (d == "media_player") svc = up ? "media_player/volume_up" : "media_player/volume_down";
  else if (d == "input_number" || d == "counter") svc = d + (up ? "/increment" : "/decrement");
  else return 0;
  int code = 200;
  for (int i = 0; i < n && code == 200; ++i) code = callService(svc, s.turn);   // one speed/volume step per click
  return code;
}

// Small JSON helpers: HA state objects are flat enough for indexOf.
static String jsonStr(const String &js, const char *key) {
  const String k = String("\"") + key + "\":\"";
  const int i = js.indexOf(k);
  if (i < 0) return "";
  const int e = js.indexOf('"', i + k.length());
  return e < 0 ? "" : js.substring(i + k.length(), e);
}

static bool jsonNum(const String &js, const char *key, float &out) {
  const String k = String("\"") + key + "\":";
  const int i = js.indexOf(k);
  if (i < 0) return false;
  const char c = js[i + k.length()];
  if (!(c == '-' || (c >= '0' && c <= '9'))) return false;   // null
  out = js.substring(i + k.length()).toFloat();
  return true;
}

static void refreshState(const char *entity) {
  String js;
  const int code = haRequest("GET", String("/api/states/") + entity, "", &js);
  if (code != 200) { setStatus(httpError(code)); return; }
  if (!js.length()) { setStatus("OK"); return; }
  String st = jsonStr(js, "state");
  st.toUpperCase();
  const String d = domainOf(entity);
  float v;
  char line[22];
  if (d == "light" && jsonNum(js, "brightness", v))
    snprintf(line, sizeof(line), "%s %d%%", st.c_str(), (int)(v * 100 / 255 + 0.5f));
  else if (d == "fan" && jsonNum(js, "percentage", v))
    snprintf(line, sizeof(line), "%s %d%%", st.c_str(), (int)v);
  else if (d == "media_player" && jsonNum(js, "volume_level", v))
    snprintf(line, sizeof(line), "%.10s vol %d%%", st.c_str(), (int)(v * 100 + 0.5f));
  else if (d == "cover" && jsonNum(js, "current_position", v))
    snprintf(line, sizeof(line), "%s %d%%", st.c_str(), (int)v);
  else
    snprintf(line, sizeof(line), "%s", st.c_str());
  setStatus(line);
}

// ---------- worker task ----------
static void workerTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

    int slot, presses, turn;
    bool refresh;
    portENTER_CRITICAL(&mux);
    slot = reqSlot; presses = reqPresses; turn = reqTurn; refresh = reqRefresh;
    reqPresses = 0; reqTurn = 0; reqRefresh = false;
    portEXIT_CRITICAL(&mux);

    if (!presses && !turn && !refresh) {
      if (wifiStarted && millis() - lastUseMs > WIFI_IDLE_OFF_MS) wifiStop();
      continue;
    }
    if (slot < 0 || slot >= slotCount) continue;
    if (haUrl.startsWith("https://")) { setStatus("Use an http:// URL"); continue; }
    if (!ensureWifi()) continue;

    const Slot &s = slots[slot];
    int code = 200;
    if (presses & 1) {   // an even number of toggles cancels out
      setStatus("...");
      code = doPress(s);
      Serial.printf("[HOME] press %s -> %d\n", s.press, code);
    }
    if (turn && code == 200) {
      code = doTurn(s, turn);
      Serial.printf("[HOME] turn %s %+d -> %d\n", s.turn, turn, code);
      if (code == 0) { setStatus("No dial action"); continue; }
    }
    if (code != 200) { setStatus(httpError(code)); continue; }
    if (presses || turn) vTaskDelay(pdMS_TO_TICKS(400));   // let the device report its new state
    refreshState(turn ? s.turn : s.press);
  }
}

static void request(int presses, int turn, bool refresh) {
  if (!homeConfigured()) {
    setStatus(wifiSsid.length() ? "Set up HA (see docs)" : "Set up Wi-Fi first");
    return;
  }
  if (!worker) {
    // 6 KB stack: HTTPClient + Strings; heap is what's tight with BLE up, not CPU.
    if (xTaskCreatePinnedToCore(workerTask, "home", 6144, nullptr, 1, &worker, 0) != pdPASS) {
      worker = nullptr;
      setStatus("No memory");
      return;
    }
  }
  portENTER_CRITICAL(&mux);
  if (reqSlot != activeSlot) { reqPresses = 0; reqTurn = 0; }   // don't apply old input to a new device
  reqSlot = activeSlot;
  reqPresses += presses;
  reqTurn = constrain(reqTurn + turn, -20, 20);
  reqRefresh |= refresh;
  portEXIT_CRITICAL(&mux);
  lastUseMs = millis();
  xTaskNotifyGive(worker);
}

// ---------- mode / input ----------
static void enterMode() {
  if (isMenuOpen()) menuClose();
  if (!modeActive) {
    Serial.println(F("[HOME] mode on"));
    ++panelVer;
  }
  modeActive = true;
  lastInputMs = millis();
  setStatus("...");
  request(0, 0, true);   // connect + show current state while the E-Ink redraws
}

void homeExit() {
  if (!modeActive) return;
  modeActive = false;
  ++panelVer;
  Serial.println(F("[HOME] mode off"));
}

void homeToggleMode() {
  if (modeActive) homeExit();
  else enterMode();
}

void homeSelectSlot(int n) {
  if (n < 1 || n > slotCount) {
    if (!modeActive) enterMode();
    char b[22];
    snprintf(b, sizeof(b), "No device on Fn+%d", n);
    setStatus(b);
    return;
  }
  if (activeSlot != n - 1) ++panelVer;   // E-Ink highlight moves
  activeSlot = n - 1;
  Serial.printf("[HOME] device %d: %s\n", n, slots[activeSlot].name);
  enterMode();
}

void homePress() {
  if (!modeActive) return;
  lastInputMs = millis();
  request(1, 0, false);
}

void homeTurn(int steps) {
  if (!modeActive || !steps) return;
  lastInputMs = millis();
  request(0, steps, false);
}

void homeLoop() {
  if (modeActive && millis() - lastInputMs > HOME_MODE_TIMEOUT_MS) {
    Serial.println(F("[HOME] timeout"));
    homeExit();
  }
}

bool homeModeActive() { return modeActive; }
int homeSlotCount() { return slotCount; }
int homeActiveSlot() { return activeSlot; }
const char *homeSlotName(int i) { return (i >= 0 && i < slotCount) ? slots[i].name : ""; }
uint32_t homeStatusVersion() { return statusVer; }
uint32_t homePanelVersion() { return panelVer; }

void homeStatusText(char *buf, size_t len) {
  portENTER_CRITICAL(&mux);
  strlcpy(buf, status, len);
  portEXIT_CRITICAL(&mux);
}
