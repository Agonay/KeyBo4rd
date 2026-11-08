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
static const unsigned long CHECK_COUNTDOWN_MS = 10UL * 1000UL;
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

// With BLE running there isn't enough RAM for Wi-Fi + TLS (measured: 60 KB free,
// 25 KB after Wi-Fi init; the TLS handshake fails). So checks always run in the
// boot path: the menu/idle trigger stores a reason in NVS and reboots.
enum CheckReason : uint8_t { CHECK_NONE = 0, CHECK_IDLE = 1, CHECK_MANUAL = 2 };

static void setCheckReason(CheckReason r) {
  Preferences p;
  p.begin(NS, false);
  p.putUChar("otaChk", r);
  p.end();
}

static CheckReason takeCheckReason() {
  Preferences p;
  p.begin(NS, false);
  const CheckReason r = (CheckReason)p.getUChar("otaChk", CHECK_NONE);
  if (r != CHECK_NONE) p.putUChar("otaChk", CHECK_NONE);
  if (p.isKey("otaPendV")) p.remove("otaPendV");  // key from older firmware
  p.end();
  return r;
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
  const String ver = toVersion > 0 ? String("v") + fwVersionString(FW_VERSION) + " -> v" + fwVersionString(toVersion) : String("");
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

  Serial.printf("[OTA] Wi-Fi start: heap %u, largest block %u\n",
                ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  const bool modeOk = WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);  // modem sleep is required for Wi-Fi + BLE coexistence
  const wl_status_t beginSt = WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[OTA] mode ok=%d begin=%d heap after init %u\n", modeOk, beginSt, ESP.getFreeHeap());
  const unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[OTA] Wi-Fi '%s' not reachable (status %d after %lu ms)\n",
                  ssid.c_str(), WiFi.status(), millis() - t0);
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

static bool fetchManifestOnce(Manifest &m) {
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
  Serial.printf("[OTA] latest v%s, running v%s\n", fwVersionString(m.version).c_str(), fwVersionString(FW_VERSION).c_str());
  return true;
}

static bool fetchManifest(Manifest &m) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt) delay(1500);
    lastError = "";
    if (fetchManifestOnce(m)) return true;
  }
  return false;
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
  Serial.printf("[OTA] firmware v%s%s\n", fwVersionString(FW_VERSION).c_str(),
                FW_VERSION == 0 ? " (dev build, auto-update off)" : "");
  const CheckReason reason = takeCheckReason();
  const bool manual = reason == CHECK_MANUAL;
  lastCheckAt = millis();
  seedWifiFromSecrets();

  // Dev builds only check when asked from the menu.
  if ((FW_VERSION == 0 && !manual) || !batteryOkForUpdate()) return;
  if (!otaHasWifiCredentials()) {
    setStatus("No WiFi set");
    if (manual) {
      updateScreen("UPDATE CHECK", -1, "No Wi-Fi configured", -1, "Hold knob at boot");
      delay(3000);
      closeUpdateScreen();
    }
    return;
  }

  lastError = "";
  if (manual) updateScreen("UPDATE CHECK", -1, "Connecting to Wi-Fi...", -1, nullptr);

  Manifest m;
  bool found = false;
  bool checked = false;
  if (wifiConnect(8000)) {
    if (manual) updateScreen("UPDATE CHECK", -1, "Asking GitHub...", -1, nullptr);
    checked = fetchManifest(m);
    found = checked && m.version > FW_VERSION;
  }

  if (found) {
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

  if (found) {
    setStatus("Failed");
    updateScreen("UPDATE FAILED", -1, lastError, -1, (String("Still on v") + fwVersionString(FW_VERSION)).c_str());
    delay(3000);
  } else if (!checked) {
    setStatus(lastError.startsWith("Wi-Fi") ? "No WiFi" : "Failed");
    if (manual) {
      updateScreen("UPDATE CHECK", -1, lastError.length() ? lastError : String("Check failed"),
                   -1, "Try again later");
      delay(3000);
    }
  } else {
    setStatus("Latest");
    if (manual) {
      updateScreen("UP TO DATE", -1, String("v") + fwVersionString(FW_VERSION) + " is the latest", -1, nullptr);
      delay(2000);
    }
  }
  if (screenActive) closeUpdateScreen();
}

// ---------- triggers while running (BLE up) ----------
static unsigned long countdownStart = 0;
static int countdownShown = -1;

static unsigned long lastActivity() {
  const unsigned long k = getLastKeyActivityTime();
  const unsigned long r = getLastKnobActivityTime();
  return (long)(k - r) > 0 ? k : r;
}

static void rebootIntoCheck(CheckReason reason) {
  setCheckReason(reason);
  if (bleKeyboard.isConnected()) bleKeyboard.releaseAll();
  delay(300);
  ESP.restart();
}

void otaCheckNow() {
  if (!otaHasWifiCredentials()) { setStatus("No WiFi set"); return; }
  if (isMenuOpen()) menuClose();
  updateScreen("UPDATE CHECK", -1, "Restarting to check...", -1, "Bluetooth reconnects");
  delay(800);
  rebootIntoCheck(CHECK_MANUAL);
}

void otaLoop() {
  if (FW_VERSION == 0) return;   // dev build: no automatic checks
  const unsigned long now = millis();
  const bool idle = now - lastActivity() >= IDLE_BEFORE_CHECK_MS && !isMenuOpen();

  // Idle check due: 10 s countdown, any key or knob use postpones it until the
  // next idle window, then reboot and check in the boot path.
  if (countdownStart) {
    if ((long)(lastActivity() - countdownStart) > 0 || isMenuOpen()) {
      Serial.println("[OTA] update check postponed (activity)");
      countdownStart = 0;
      closeUpdateScreen();
      return;
    }
    const unsigned long elapsed = now - countdownStart;
    if (elapsed >= CHECK_COUNTDOWN_MS) {
      updateScreen("UPDATE CHECK", -1, "Restarting to check...", 100, nullptr);
      rebootIntoCheck(CHECK_IDLE);
    }
    const int left = (CHECK_COUNTDOWN_MS - elapsed + 999) / 1000;
    if (left != countdownShown) {
      countdownShown = left;
      updateScreen("UPDATE CHECK", -1, String("Restarting in ") + left + "s",
                   (int)(elapsed * 100 / CHECK_COUNTDOWN_MS), "Press any key to wait");
    }
    return;
  }

  if (!idle || now - lastCheckAt < MIN_CHECK_INTERVAL_MS) return;
  if (!otaHasWifiCredentials() || !batteryOkForUpdate()) return;
  lastCheckAt = now;
  countdownStart = now;
  countdownShown = -1;
  Serial.println("[OTA] idle: rebooting to check for updates in 10 s unless a key is pressed");
}
