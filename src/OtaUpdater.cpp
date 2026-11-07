#include "OtaUpdater.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include "GithubRootCAs.h"
#include "DisplayManager.h"
#include "KeyboardMatrix.h"
#include "RotaryEncoder.h"
#include "MenuManager.h"
#include "BluetoothHID.h"

#define OTA_REPO "Agonay/KeyBo4rd"
static const char *MANIFEST_URL =
  "https://github.com/" OTA_REPO "/releases/latest/download/version.json";

static const unsigned long IDLE_BEFORE_CHECK_MS = 4UL * 60UL * 1000UL;
static const unsigned long MIN_CHECK_INTERVAL_MS = 60UL * 60UL * 1000UL;
static const uint32_t MIN_FREE_HEAP_FOR_TLS = 60000;
static const char *NS = "kbd";

struct Manifest {
  long version = -1;
  long size = 0;
  String md5;
};

// ---------- credentials (NVS, survive OTA) ----------
String otaSavedSsid() {
  Preferences p;
  p.begin(NS, true);
  String s = p.getString("wifiSsid", "");
  p.end();
  return s;
}

bool otaHasWifiCredentials() { return otaSavedSsid().length() > 0; }

void otaSaveWifiCredentials(const String &ssid, const String &pass) {
  Preferences p;
  p.begin(NS, false);
  p.putString("wifiSsid", ssid);
  p.putString("wifiPass", pass);
  p.end();
}

static void setPending(bool v) {
  Preferences p;
  p.begin(NS, false);
  p.putBool("otaPend", v);
  p.end();
}

// Local USB builds can carry credentials in the gitignored src/WifiSecrets.h;
// they are copied to NVS so later OTA builds (which lack the file) keep them.
#if __has_include("WifiSecrets.h")
#include "WifiSecrets.h"
#endif
static void seedWifiFromSecrets() {
#if defined(WIFI_SSID) && defined(WIFI_PASS)
  if (sizeof(WIFI_SSID) <= 1) return;  // empty placeholder
  Preferences p;
  p.begin(NS, true);
  const bool same = p.getString("wifiSsid", "") == WIFI_SSID && p.getString("wifiPass", "") == WIFI_PASS;
  p.end();
  if (!same) {
    otaSaveWifiCredentials(WIFI_SSID, WIFI_PASS);
    Serial.printf("[OTA] saved Wi-Fi '%s' from WifiSecrets.h\n", WIFI_SSID);
  }
#endif
}

// Battery is simulated for now (BatteryManager); hook the real ADC check in here.
static bool batteryOkForUpdate() { return true; }

// ---------- Wi-Fi ----------
static bool wifiConnect(unsigned long timeoutMs) {
  Preferences p;
  p.begin(NS, true);
  String ssid = p.getString("wifiSsid", "");
  String pass = p.getString("wifiPass", "");
  p.end();
  if (!ssid.length()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);  // modem sleep is required for Wi-Fi + BLE coexistence
  WiFi.begin(ssid.c_str(), pass.c_str());
  const unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[OTA] Wi-Fi '%s' not reachable\n", ssid.c_str());
    return false;
  }
  Serial.printf("[OTA] Wi-Fi connected, IP %s\n", WiFi.localIP().toString().c_str());
  return true;
}

static void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ---------- manifest ----------
static long jsonInt(const String &s, const char *key) {
  String k = String("\"") + key + "\":";
  int i = s.indexOf(k);
  if (i < 0) return -1;
  return s.substring(i + k.length()).toInt();
}

static String jsonStr(const String &s, const char *key) {
  String k = String("\"") + key + "\":\"";
  int i = s.indexOf(k);
  if (i < 0) return "";
  int start = i + k.length();
  int end = s.indexOf('"', start);
  return end < 0 ? "" : s.substring(start, end);
}

static bool fetchManifest(Manifest &m) {
  WiFiClientSecure client;
  client.setCACert(GITHUB_ROOT_CAS);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  if (!http.begin(client, MANIFEST_URL)) return false;
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] manifest HTTP %d\n", code);
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();
  m.version = jsonInt(body, "version");
  m.size = jsonInt(body, "size");
  m.md5 = jsonStr(body, "md5");
  if (m.version < 0 || m.size <= 0 || m.md5.length() != 32) {
    Serial.println("[OTA] manifest malformed");
    return false;
  }
  Serial.printf("[OTA] latest v%ld, running v%d\n", m.version, FW_VERSION);
  return true;
}

// ---------- OLED feedback ----------
static void oledShow(const char *line1, const String &line2, int pct) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 8);
  oled.println(line1);
  oled.setCursor(0, 24);
  oled.println(line2);
  if (pct >= 0) {
    oled.drawRect(0, 44, 128, 10, SSD1306_WHITE);
    oled.fillRect(2, 46, (124 * pct) / 100, 6, SSD1306_WHITE);
  }
  oled.display();
}

// ---------- download + flash ----------
static bool downloadAndFlash(const Manifest &m) {
  const String url = String("https://github.com/" OTA_REPO "/releases/download/fw-") +
                     m.version + "/firmware.bin";
  WiFiClientSecure client;
  client.setCACert(GITHUB_ROOT_CAS);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] firmware HTTP %d\n", code);
    http.end();
    return false;
  }
  const int len = http.getSize();
  if (len > 0 && len != m.size) {
    Serial.printf("[OTA] size mismatch %d != %ld\n", len, m.size);
    http.end();
    return false;
  }
  if (!Update.begin(m.size, U_FLASH)) {
    Update.printError(Serial);
    http.end();
    return false;
  }
  Update.setMD5(m.md5.c_str());

  WiFiClient *stream = http.getStreamPtr();
  static uint8_t buf[2048];
  long written = 0;
  int lastPct = -1;
  unsigned long lastData = millis();
  const String label = String("v") + FW_VERSION + " -> v" + m.version;
  while (written < m.size) {
    const size_t avail = stream->available();
    if (avail) {
      size_t want = min(avail, sizeof(buf));
      want = min(want, (size_t)(m.size - written));
      const int n = stream->readBytes(buf, want);
      if (n <= 0 || Update.write(buf, n) != (size_t)n) break;
      written += n;
      lastData = millis();
      const int pct = (int)((written * 100) / m.size);
      if (pct != lastPct) { lastPct = pct; oledShow("Updating firmware", label, pct); }
    } else if (!stream->connected() || millis() - lastData > 15000) {
      break;
    } else {
      delay(1);
    }
  }
  http.end();
  if (written != m.size) {
    Serial.printf("[OTA] download incomplete %ld/%ld\n", written, m.size);
    Update.abort();
    return false;
  }
  if (!Update.end()) {  // verifies the MD5 from version.json
    Update.printError(Serial);
    return false;
  }
  return true;
}

// ---------- boot path (no BLE yet, full RAM) ----------
static unsigned long lastCheckAt = 0;

void otaBootCheck() {
  Serial.printf("[OTA] firmware v%d%s\n", FW_VERSION,
                FW_VERSION == 0 ? " (dev build, auto-update off)" : "");
  Preferences p;
  p.begin(NS, true);
  const bool pending = p.getBool("otaPend", false);
  p.end();
  if (pending) setPending(false);
  lastCheckAt = millis();
  seedWifiFromSecrets();

  if (FW_VERSION == 0 || !otaHasWifiCredentials() || !batteryOkForUpdate()) return;

  bool drew = false;
  if (pending) { oledShow("Update found", "Connecting...", -1); drew = true; }
  if (wifiConnect(8000)) {
    Manifest m;
    if (fetchManifest(m) && m.version > FW_VERSION) {
      drew = true;
      oledShow("Updating firmware", String("to v") + m.version, 0);
      if (downloadAndFlash(m)) {
        oledShow("Update installed", "Rebooting...", 100);
        delay(800);
        ESP.restart();
      }
      oledShow("Update failed", "Keeping current FW", -1);
      delay(2000);
    }
  }
  wifiOff();
  if (drew) {
    oled.clearDisplay();
    oled.display();
  }
}

// ---------- idle path (BLE running) ----------
static volatile bool checkRunning = false;
static volatile bool restartRequested = false;

static void idleCheckTask(void *) {
  bool newer = false;
  const uint32_t heap = ESP.getFreeHeap();
  if (heap < MIN_FREE_HEAP_FOR_TLS) {
    Serial.printf("[OTA] skip idle check, free heap %u\n", heap);
  } else if (wifiConnect(10000)) {
    Manifest m;
    newer = fetchManifest(m) && m.version > FW_VERSION;
  }
  wifiOff();
  if (newer) {
    setPending(true);
    restartRequested = true;
  }
  checkRunning = false;
  vTaskDelete(nullptr);
}

static unsigned long lastActivity() {
  const unsigned long k = getLastKeyActivityTime();
  const unsigned long r = getLastKnobActivityTime();
  return (long)(k - r) > 0 ? k : r;
}

void otaLoop() {
  if (FW_VERSION == 0 || checkRunning) return;
  const unsigned long now = millis();
  const bool idle = now - lastActivity() >= IDLE_BEFORE_CHECK_MS && !isMenuOpen();

  if (restartRequested) {
    if (!idle) return;  // user came back: wait for the next idle window
    Serial.println("[OTA] newer firmware found, rebooting to install");
    if (bleKeyboard.isConnected()) bleKeyboard.releaseAll();
    delay(100);
    ESP.restart();
  }

  if (!idle || now - lastCheckAt < MIN_CHECK_INTERVAL_MS) return;
  if (!otaHasWifiCredentials() || !batteryOkForUpdate()) return;
  lastCheckAt = now;
  checkRunning = true;
  if (xTaskCreatePinnedToCore(idleCheckTask, "otaCheck", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    checkRunning = false;
  }
}
