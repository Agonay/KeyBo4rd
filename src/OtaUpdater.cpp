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
static const unsigned long INSTALL_COUNTDOWN_MS = 10UL * 1000UL;
static const uint32_t MIN_FREE_HEAP_FOR_TLS = 60000;
static const char *NS = "kbd";

struct Manifest {
  long version = -1;
  long size = 0;
  String md5;
  String notes;   // commit title, ASCII-only
};

static String lastError;
static char statusText[16] = "";
static char lastIp[16] = "off";
const char *otaStatusText() { return statusText; }
const char *otaLastIp() { return lastIp; }
static void setStatus(const char *s) { strlcpy(statusText, s, sizeof(statusText)); }

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

// Version found by the idle check; the boot path installs it. 0 = none.
static void setPendingVersion(long v) {
  Preferences p;
  p.begin(NS, false);
  p.putLong("otaPendV", v);
  p.end();
}

static long takePendingVersion() {
  Preferences p;
  p.begin(NS, false);
  const long v = p.getLong("otaPendV", 0);
  if (v) p.putLong("otaPendV", 0);
  p.end();
  return v;
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

// ---------- update screen (OLED 128x64) ----------
static volatile bool screenActive = false;
bool otaScreenActive() { return screenActive; }

static void drawCentered(const String &s, int y, uint8_t size) {
  oled.setTextSize(size);
  const int w = s.length() * 6 * size;
  oled.setCursor(w >= 128 ? 0 : (128 - w) / 2, y);
  oled.print(s);
}

static String screenNotes;  // release notes shown on the update screen

// Layout without notes: title bar / big "vA -> vB" / status / progress bar / footer.
// With notes:           title bar / "vA -> vB" / 2 lines of notes / status / bar / footer.
static void updateScreen(const char *title, long toVersion, const String &status,
                         int pct, const char *footer) {
  screenActive = true;
  oled.clearDisplay();
  oled.fillRect(0, 0, 128, 11, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  drawCentered(title, 2, 1);
  oled.setTextColor(SSD1306_WHITE);
  const String ver = toVersion > 0 ? String("v") + FW_VERSION + " -> v" + toVersion : String("");
  int statusY = 34, barY = 44;
  if (screenNotes.length() && toVersion > 0) {
    drawCentered(ver, 13, 1);
    oled.setTextSize(1);
    oled.setCursor(0, 23);
    oled.print(screenNotes.substring(0, 21));
    if (screenNotes.length() > 21) {
      String l2 = screenNotes.substring(21, 42);
      if (screenNotes.length() > 42) l2 = l2.substring(0, 18) + "...";
      oled.setCursor(0, 31);
      oled.print(l2);
    }
    statusY = 40; barY = 49;
  } else if (ver.length()) {
    drawCentered(ver, 15, ver.length() <= 10 ? 2 : 1);
  }
  drawCentered(status, statusY, 1);
  if (pct >= 0) {
    oled.drawRect(4, barY, 120, 6, SSD1306_WHITE);
    oled.fillRect(5, barY + 1, (118 * constrain(pct, 0, 100)) / 100, 4, SSD1306_WHITE);
  }
  if (footer) drawCentered(footer, 57, 1);
  oled.display();
}

static void closeUpdateScreen() {
  screenActive = false;
  oled.clearDisplay();
  oled.display();
  oledForceRedraw();
}

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
    lastError = "Wi-Fi not reachable";
    return false;
  }
  strlcpy(lastIp, WiFi.localIP().toString().c_str(), sizeof(lastIp));
  Serial.printf("[OTA] Wi-Fi connected, IP %s\n", lastIp);
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

// Reads a JSON string value, decoding \" and \\ escapes; non-ASCII becomes '?'
// (the OLED font has no accents).
static String jsonStr(const String &s, const char *key) {
  String k = String("\"") + key + "\":\"";
  int i = s.indexOf(k);
  if (i < 0) return "";
  String out;
  for (int p = i + k.length(); p < (int)s.length(); p++) {
    char c = s[p];
    if (c == '"') break;
    if (c == '\\' && p + 1 < (int)s.length()) {
      c = s[++p];
      if (c == 'n' || c == 't') c = ' ';
      else if (c == 'u') { p += 4; c = '?'; }
    }
    if ((uint8_t)c >= 0x80) {
      if (((uint8_t)c & 0xC0) == 0x80) continue;  // UTF-8 continuation byte
      c = '?';
    }
    out += c;
  }
  return out;
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
    lastError = String("Server error ") + code;
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();
  m.version = jsonInt(body, "version");
  m.size = jsonInt(body, "size");
  m.md5 = jsonStr(body, "md5");
  m.notes = jsonStr(body, "notes");
  if (m.version < 0 || m.size <= 0 || m.md5.length() != 32) {
    Serial.println("[OTA] manifest malformed");
    lastError = "Bad release info";
    return false;
  }
  Serial.printf("[OTA] latest v%ld, running v%d\n", m.version, FW_VERSION);
  return true;
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
  if (!http.begin(client, url)) { lastError = "Bad URL"; return false; }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] firmware HTTP %d\n", code);
    lastError = String("Download error ") + code;
    http.end();
    return false;
  }
  const int len = http.getSize();
  if (len > 0 && len != m.size) {
    Serial.printf("[OTA] size mismatch %d != %ld\n", len, m.size);
    lastError = "Size mismatch";
    http.end();
    return false;
  }
  if (!Update.begin(m.size, U_FLASH)) {
    Update.printError(Serial);
    lastError = "No room for update";
    http.end();
    return false;
  }
  Update.setMD5(m.md5.c_str());

  WiFiClient *stream = http.getStreamPtr();
  static uint8_t buf[2048];
  long written = 0;
  int lastPct = -1;
  unsigned long lastData = millis();
  const long totalKb = m.size / 1024;
  // Don't trust connected()/available() on the TLS stream (they can report a
  // momentary gap as a disconnect); keep reading until data stops for 20 s.
  while (written < m.size) {
    const size_t want = min(sizeof(buf), (size_t)(m.size - written));
    const int n = stream->readBytes(buf, want);
    if (n > 0) {
      if (Update.write(buf, n) != (size_t)n) {
        Update.printError(Serial);
        lastError = "Flash write failed";
        break;
      }
      written += n;
      lastData = millis();
      const int pct = (int)((written * 100) / m.size);
      if (pct != lastPct) {
        lastPct = pct;
        updateScreen("FIRMWARE UPDATE", m.version,
                     String("Downloading ") + written / 1024 + "/" + totalKb + " KB",
                     pct, "Don't unplug");
      }
    } else if (millis() - lastData > 20000) {
      Serial.println("[OTA] download stalled (no data for 20 s)");
      lastError = "Download stalled";
      break;
    }
  }
  http.end();
  if (written != m.size) {
    Serial.printf("[OTA] download incomplete %ld/%ld\n", written, m.size);
    if (!lastError.length()) lastError = "Download incomplete";
    Update.abort();
    return false;
  }
  updateScreen("FIRMWARE UPDATE", m.version, "Verifying...", 100, "Don't unplug");
  if (!Update.end()) {  // verifies the MD5 from version.json
    Update.printError(Serial);
    lastError = "Verification failed";
    return false;
  }
  return true;
}

// ---------- boot path (no BLE yet, full RAM) ----------
static unsigned long lastCheckAt = 0;

void otaBootCheck() {
  Serial.printf("[OTA] firmware v%d%s\n", FW_VERSION,
                FW_VERSION == 0 ? " (dev build, auto-update off)" : "");
  const long pending = takePendingVersion();
  lastCheckAt = millis();
  seedWifiFromSecrets();

  if (FW_VERSION == 0 || !otaHasWifiCredentials() || !batteryOkForUpdate()) return;

  lastError = "";
  if (pending > FW_VERSION)
    updateScreen("FIRMWARE UPDATE", pending, "Connecting to Wi-Fi...", -1, "Don't unplug");

  Manifest m;
  bool tried = false;
  if (wifiConnect(8000) && fetchManifest(m) && m.version > FW_VERSION) {
    tried = true;
    screenNotes = m.notes;
    updateScreen("FIRMWARE UPDATE", m.version, "Starting download...", 0, "Don't unplug");
    if (downloadAndFlash(m)) {
      wifiOff();
      updateScreen("UPDATE COMPLETE", m.version, "Restarting...", 100, nullptr);
      delay(1200);
      ESP.restart();
    }
  }
  wifiOff();

  if (tried || pending > FW_VERSION) {
    updateScreen("UPDATE FAILED", -1, lastError.length() ? lastError : String("No update found"),
                 -1, (String("Still on v") + FW_VERSION).c_str());
    delay(3000);
  }
  if (screenActive) closeUpdateScreen();
}

// ---------- idle path (BLE running) ----------
static volatile bool checkRunning = false;
static volatile long foundVersion = 0;      // set by the background task
static volatile bool manualCheck = false;   // started from the menu
static String foundNotes;
static unsigned long countdownStart = 0;
static int countdownShown = -1;

static void idleCheckTask(void *) {
  long newer = 0;
  const uint32_t heap = ESP.getFreeHeap();
  if (heap < MIN_FREE_HEAP_FOR_TLS) {
    Serial.printf("[OTA] skip check, free heap %u\n", heap);
    setStatus("Low memory");
  } else if (!wifiConnect(10000)) {
    setStatus("No WiFi");
  } else {
    Manifest m;
    if (!fetchManifest(m)) {
      setStatus("Failed");
    } else if (m.version > FW_VERSION) {
      newer = m.version;
      foundNotes = m.notes;
      char b[16];
      snprintf(b, sizeof(b), "v%ld found", m.version);
      setStatus(b);
    } else {
      setStatus("Latest");
    }
  }
  wifiOff();
  if (newer) foundVersion = newer;
  checkRunning = false;
  vTaskDelete(nullptr);
}

static unsigned long lastActivity() {
  const unsigned long k = getLastKeyActivityTime();
  const unsigned long r = getLastKnobActivityTime();
  return (long)(k - r) > 0 ? k : r;
}

static bool startCheckTask() {
  checkRunning = true;
  setStatus("Checking");
  if (xTaskCreatePinnedToCore(idleCheckTask, "otaCheck", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    checkRunning = false;
    setStatus("Failed");
    return false;
  }
  return true;
}

void otaCheckNow() {
  if (checkRunning) return;
  if (!otaHasWifiCredentials()) { setStatus("No WiFi set"); return; }
  manualCheck = true;
  lastCheckAt = millis();
  if (!startCheckTask()) manualCheck = false;
}

static void installNow(long version) {
  setPendingVersion(version);
  screenNotes = foundNotes;
  updateScreen("FIRMWARE UPDATE", version, "Restarting to install", 100, "Don't unplug");
  if (bleKeyboard.isConnected()) bleKeyboard.releaseAll();
  delay(500);
  ESP.restart();
}

void otaLoop() {
  if (checkRunning) return;
  const unsigned long now = millis();

  // Manual check from the menu: install immediately, no countdown.
  if (manualCheck) {
    manualCheck = false;
    if (foundVersion) {
      if (isMenuOpen()) menuClose();
      installNow(foundVersion);
    }
    return;
  }

  if (FW_VERSION == 0) return;   // dev build: no automatic checks
  const bool idle = now - lastActivity() >= IDLE_BEFORE_CHECK_MS && !isMenuOpen();

  // An update was found: show a countdown; any key or knob use postpones it
  // until the next idle window.
  if (foundVersion) {
    if (!countdownStart) {
      if (!idle) return;
      countdownStart = now;
      countdownShown = -1;
      Serial.printf("[OTA] v%ld ready, installing in %lus unless a key is pressed\n",
                    (long)foundVersion, INSTALL_COUNTDOWN_MS / 1000);
    }
    if ((long)(lastActivity() - countdownStart) > 0 || isMenuOpen()) {
      Serial.println("[OTA] install postponed (activity)");
      countdownStart = 0;
      closeUpdateScreen();
      return;
    }
    const unsigned long elapsed = now - countdownStart;
    if (elapsed >= INSTALL_COUNTDOWN_MS) installNow(foundVersion);
    const int left = (INSTALL_COUNTDOWN_MS - elapsed + 999) / 1000;
    if (left != countdownShown) {
      countdownShown = left;
      screenNotes = foundNotes;
      updateScreen("UPDATE READY", foundVersion, String("Installing in ") + left + "s",
                   (int)(elapsed * 100 / INSTALL_COUNTDOWN_MS), "Press any key to wait");
    }
    return;
  }

  if (!idle || now - lastCheckAt < MIN_CHECK_INTERVAL_MS) return;
  if (!otaHasWifiCredentials() || !batteryOkForUpdate()) return;
  lastCheckAt = now;
  startCheckTask();
}
