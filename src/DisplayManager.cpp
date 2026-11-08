#include "DisplayManager.h"
#include "BatteryManager.h"
#include "BluetoothHID.h"
#include "MenuManager.h"
#include "KeyboardMatrix.h"
#include <GxEPD2_BW.h>
#include "totoro_200x200_1b.h"
#include "HardwareConfig.h"
#include "OtaUpdater.h"
#include "RotaryEncoder.h"
#include "HomeControl.h"
#include "EinkAnimation.h"

#define ENABLE_GxEPD2_GFX 0

// OLED
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
#define OLED_ADDR 0x3C

// E-Ink
#define CS_PIN   5
#define BUSY_PIN 15
#define RES_PIN  17
#define DC_PIN   16

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> einkDisplay(GxEPD2_154_D67(CS_PIN, DC_PIN, RES_PIN, BUSY_PIN));

// ---------- idle mode tracking ----------
static MenuIdleMode currentIdleMode = (MenuIdleMode)255; // sentinel: force first render
static bool einkInitialized = false;
static bool einkIsAwake = false;
static bool menuWasOpen = false;
static uint8_t lastOledBrt = 100;

static unsigned long lastIdleRenderMs = 0;
static const unsigned long idleCooldownMs = 300000; // 5 min between full redraws (e-ink is slow)

// Clock state: only redraw when second changes
static unsigned long lastDrawnSecond = 0;
static bool clockNeedsUpdate = false;

// SPI owns GPIO18 only between einkWake() and einkSleep(); the rest of the time
// it's the encoder's CLK input (see HardwareConfig.h).
static bool einkBusHeld = false;

void einkAcquireBus() {
  if (einkBusHeld) return;
#if EINK_SHARES_ENCODER_PIN
  encoderSuspend();
#endif
  SPI.begin(EINK_SCK, -1, EINK_MOSI, CS_PIN);
  einkBusHeld = true;
}

void einkReleaseBus() {
#if EINK_SHARES_ENCODER_PIN
  if (!einkBusHeld) return;
  SPI.end();          // detaches SCK/MOSI and leaves them as inputs
  einkBusHeld = false;
  encoderResume();
#endif
}

bool einkReady() { return einkInitialized; }

void einkWake() {
  if (!einkInitialized) return;
  einkAcquireBus();
  if (!einkIsAwake) {
    einkDisplay.init(115200, true, 50, false);
    einkIsAwake = true;
  }
}

// Like einkWake(), but without forcing a full refresh first: for continuing with
// partial refreshes when the caller knows what's on the glass.
void einkWakeKeep() {
  if (!einkInitialized) return;
  einkAcquireBus();
  if (!einkIsAwake) {
    einkDisplay.init(115200, false, 50, false);
    einkIsAwake = true;
  }
}

void einkSleep() {
  if (einkIsAwake) {
    einkAcquireBus();   // the animation releases SPI between frames; hibernate needs it
    einkDisplay.hibernate();
    einkIsAwake = false;
  }
  einkReleaseBus();
}

static void serviceKeyboardDuringRefresh(const void*) {
  scanKeyboardMatrix();
  handleButtonPress();   // the button (GPIO13) isn't shared, so presses still count
  delay(1);
}

// ---------- OLED drawing primitives ----------
static const char *DEVICE_NAME = "K3YBo4rd";

static int textWidth(const char *s, uint8_t size) { return (int)strlen(s) * 6 * size; }

static void printAt(int x, int y, const char *s, uint8_t size = 1, uint16_t color = SSD1306_WHITE) {
  oled.setTextSize(size);
  oled.setTextColor(color);
  oled.setCursor(x, y);
  oled.print(s);
}

static void printCentered(int y, const char *s, uint8_t size = 1, uint16_t color = SSD1306_WHITE) {
  int w = textWidth(s, size);
  printAt(w >= 128 ? 0 : (128 - w) / 2, y, s, size, color);
}

static void printRight(int xRight, int y, const char *s, uint8_t size = 1) {
  printAt(xRight - textWidth(s, size), y, s, size);
}

// Bluetooth rune, scalable: h = total height, top-left of its bounding box at (cx - h/4, y0).
static void drawBtRune(int cx, int y0, int h) {
  const int w = max(2, h / 4);
  const int y1 = y0 + h - 1;
  const int q1 = y0 + h / 4, q3 = y0 + (3 * h) / 4;
  oled.drawLine(cx, y0, cx, y1, SSD1306_WHITE);
  oled.drawLine(cx, y0, cx + w, q1, SSD1306_WHITE);
  oled.drawLine(cx + w, q1, cx - w, q3, SSD1306_WHITE);
  oled.drawLine(cx, y1, cx + w, q3, SSD1306_WHITE);
  oled.drawLine(cx + w, q3, cx - w, q1, SSD1306_WHITE);
}

// 15x8 battery: body 13x8, tip 2x4.
static void drawBatteryIcon(int x, int y, int pct) {
  oled.drawRect(x, y, 13, 8, SSD1306_WHITE);
  oled.fillRect(x + 13, y + 2, 2, 4, SSD1306_WHITE);
  int fill = (constrain(pct, 0, 100) * 9 + 50) / 100;
  if (pct > 0 && fill == 0) fill = 1;
  oled.fillRect(x + 2, y + 2, fill, 4, SSD1306_WHITE);
}

// ---------- boot splash ----------
static void drawSplashFrame(int nameChars, bool cursorOn, int wavePos, int pct, const char *status) {
  oled.clearDisplay();

  // Row of 12 keycaps; the ones near wavePos are "pressed" (filled, 1 px lower).
  for (int i = 0; i < 12; ++i) {
    const int x = 5 + i * 10;
    const bool down = wavePos >= 0 && abs(i - wavePos) <= 1;
    if (down) oled.fillRoundRect(x, 3, 8, 7, 1, SSD1306_WHITE);
    else      oled.drawRoundRect(x, 2, 8, 7, 1, SSD1306_WHITE);
  }

  // Name, typed out.
  char name[12];
  snprintf(name, sizeof(name), "%.*s", nameChars, DEVICE_NAME);
  const int nameX = (128 - textWidth(DEVICE_NAME, 2)) / 2;
  printAt(nameX, 17, name, 2);
  if (cursorOn) oled.fillRect(nameX + nameChars * 12, 17, 2, 14, SSD1306_WHITE);

  if (pct >= 0) {
    oled.drawRoundRect(14, 37, 100, 6, 2, SSD1306_WHITE);
    oled.fillRect(16, 39, (96 * constrain(pct, 0, 100)) / 100, 2, SSD1306_WHITE);
  }
  if (status) printCentered(46, status);

  char fw[12];
  if (FW_VERSION == 0) snprintf(fw, sizeof(fw), "dev build");
  else { fw[0] = 'v'; fwVersionLabel(FW_VERSION, fw + 1, sizeof(fw) - 1); }
  printCentered(57, fw);
  oled.display();
}

static void playBootAnimation() {
  const int n = strlen(DEVICE_NAME);
  for (int i = 0; i <= n; ++i) {
    drawSplashFrame(i, true, i * 12 / n, -1, nullptr);
    delay(65);
  }
  for (int i = 0; i < 4; ++i) {          // blink the cursor while the bar fills
    drawSplashFrame(n, i % 2 == 0, -1, (i + 1) * 5, nullptr);
    delay(90);
  }
}

void bootSplashStatus(const char *status, int pct) {
  drawSplashFrame(strlen(DEVICE_NAME), false, -1, pct, status);
}

void initDisplay() {
  delay(100);   // let the OLED supply settle

  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oled.ssd1306_command(0xD9); oled.ssd1306_command(0xF1);
  oled.ssd1306_command(SSD1306_SETDISPLAYCLOCKDIV);
  oled.ssd1306_command(0xF0);
  oled.cp437(true);
  applyOledBrightness(getSettings().oledBrightness);
  playBootAnimation();
  bootSplashStatus("Starting...", 20);
#if ENABLE_EINK
  einkDisplay.epd2.setBusyCallback(serviceKeyboardDuringRefresh);
  // E-Ink boot: show the idle scene (rain or Totoro), then hibernate (encoder isn't attached yet)
  einkInitialized = true;
  if (getSettings().einkIdleMode == IdleAnimation) {
    einkAnimBootFrame();
    currentIdleMode = IdleAnimation;
  } else {
    einkWake();
    einkDisplay.setRotation(0);
    einkDisplay.setFullWindow();
    int x = (einkDisplay.width()  - totoro_200x200_1b_width)  / 2;
    int y = (einkDisplay.height() - totoro_200x200_1b_height) / 2;
    einkDisplay.firstPage();
    do {
      einkDisplay.fillScreen(GxEPD_WHITE);
      einkDisplay.drawBitmap(x, y, totoro_200x200_1b, totoro_200x200_1b_width, totoro_200x200_1b_height, GxEPD_BLACK);
    } while (einkDisplay.nextPage());
    einkSleep();
    currentIdleMode = IdleTotoro;      // the panel shows Totoro now; other modes redraw on the first idle
  }
  Serial.println(F("[EINK] boot image drawn"));

  lastOledBrt     = getSettings().oledBrightness;
  lastDrawnSecond = 0;
#endif
}

void applyOledBrightness(uint8_t percent) {
  uint8_t val = map(percent, 0, 100, 0, 255);
  oled.ssd1306_command(0x81); // set contrast
  oled.ssd1306_command(val);
}

// ---------- home screen: typing stats ----------
// One bucket per second of key presses; WPM = presses in the window / 5 chars per word.
static const int WPM_WINDOW_S = 15;
static uint8_t pressBuckets[WPM_WINDOW_S];
static int bucketIdx = 0;
static uint32_t lastPressCount = 0;
static unsigned long lastStatTick = 0;
static int wpm = 0;

static const int SPARK_N = 64;           // 1 px per sample, one sample every 2 s (~2 min)
static uint8_t spark[SPARK_N];
static int sparkHead = 0;
static uint8_t sparkSubTick = 0;

// Returns true once per second, when the stats change.
static bool updateTypingStats(unsigned long now) {
  if (now - lastStatTick < 1000) return false;
  lastStatTick = now;
  const uint32_t count = getKeyPressCount();
  pressBuckets[bucketIdx] = (uint8_t)min<uint32_t>(255, count - lastPressCount);
  bucketIdx = (bucketIdx + 1) % WPM_WINDOW_S;
  lastPressCount = count;
  int sum = 0;
  for (int i = 0; i < WPM_WINDOW_S; ++i) sum += pressBuckets[i];
  wpm = min(999, sum * 60 / WPM_WINDOW_S / 5);
  if (++sparkSubTick >= 2) {
    sparkSubTick = 0;
    spark[sparkHead] = (uint8_t)min(255, wpm);
    sparkHead = (sparkHead + 1) % SPARK_N;
  }
  return true;
}

// ---------- home screen: toasts (knob feedback) ----------
static char toastText[16];
static unsigned long toastUntil = 0;
static int toastVolume = 0;              // net volume steps while a volume toast is showing
static bool toastDirty = false;
static const unsigned long TOAST_MS = 1100;

void homeToast(const char *text) {
  snprintf(toastText, sizeof(toastText), "%s", text);
  toastVolume = 0;
  toastUntil = millis() + TOAST_MS;
  toastDirty = true;
}

void homeToastVolume(int steps) {
  if (millis() >= toastUntil) toastVolume = 0;
  toastVolume = constrain(toastVolume + steps, -99, 99);
  if (toastVolume == 0) snprintf(toastText, sizeof(toastText), "VOL 0");
  else snprintf(toastText, sizeof(toastText), "VOL %c%d", toastVolume > 0 ? '+' : '-', abs(toastVolume));
  toastUntil = millis() + TOAST_MS;
  toastDirty = true;
}

// ---------- home screen: layout ----------
//  0..8    status bar: BT state (left), battery (right)
//  11..50  body: WPM + history graph when connected, pairing animation otherwise
//  53..63  footer: CAPS badge, key count, uptime
static void drawStatusBar(bool conn, int batPct, bool showBat, bool lowBlinkOff) {
  drawBtRune(3, 0, 8);
  printAt(10, 0, conn ? "Connected" : "Pairing");
  if (showBat) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%d%%", batPct);
    printRight(110, 0, buf);
    if (!lowBlinkOff) drawBatteryIcon(113, 0, batPct);
  }
  oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);
}

static void drawTypingBody() {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", wpm);
  const int numW = textWidth(buf, 3);
  printAt(max(0, (54 - numW) / 2), 15, buf, 3);
  printAt((54 - textWidth("WPM", 1)) / 2, 42, "WPM");

  // History graph, newest sample on the right.
  const int gx = 62, gTop = 14, gBot = 49, gh = gBot - gTop;
  int peak = 40;
  for (int i = 0; i < SPARK_N; ++i) peak = max(peak, (int)spark[i]);
  for (int x = gx; x < gx + 66; x += 4) oled.drawPixel(x, gTop, SSD1306_WHITE);  // dotted ceiling
  oled.drawFastHLine(gx, gBot, 66, SSD1306_WHITE);
  for (int i = 0; i < SPARK_N; ++i) {
    const int v = spark[(sparkHead + i) % SPARK_N];
    if (!v) continue;
    const int h = max(1, v * gh / peak);
    oled.drawFastVLine(gx + 1 + i, gBot - h, h, SSD1306_WHITE);
  }
  snprintf(buf, sizeof(buf), "%d", peak);
  oled.fillRect(127 - textWidth(buf, 1), gTop, textWidth(buf, 1) + 1, 8, SSD1306_BLACK);
  printRight(128, gTop, buf);   // graph scale
}

static void drawPairingBody(uint8_t frame) {
  // Rune with radio waves pulsing outwards.
  drawBtRune(18, 17, 28);
  for (int i = 0; i < 3; ++i) {
    if (i < (frame % 4)) oled.drawCircleHelper(22, 31, 9 + i * 5, 0x2 | 0x4, SSD1306_WHITE);
  }
  char dots[8];
  snprintf(dots, sizeof(dots), "PAIRING%.*s", frame % 4, "...");
  printAt(48, 17, dots);
  printAt(48, 30, "Find me as");
  printAt(48, 40, DEVICE_NAME);
}

static void drawToast() {
  oled.fillRoundRect(6, 16, 116, 30, 4, SSD1306_WHITE);
  const uint8_t size = textWidth(toastText, 2) <= 108 ? 2 : 1;
  printCentered(size == 2 ? 24 : 27, toastText, size, SSD1306_BLACK);
}

static void drawFooter(bool caps, uint32_t keys, unsigned long upMin) {
  oled.drawFastHLine(0, 52, 128, SSD1306_WHITE);
  if (caps) {
    oled.fillRoundRect(0, 54, 29, 10, 2, SSD1306_WHITE);
    printAt(3, 55, "CAPS", 1, SSD1306_BLACK);
  } else {
    oled.drawRoundRect(0, 54, 29, 10, 2, SSD1306_WHITE);
    printAt(3, 55, "caps");
  }
  char buf[16];
  if (keys < 100000) snprintf(buf, sizeof(buf), "%lu keys", (unsigned long)keys);
  else snprintf(buf, sizeof(buf), "%luk keys", (unsigned long)(keys / 1000));
  printAt(34, 55, buf);
  snprintf(buf, sizeof(buf), "%lu:%02lu", upMin / 60, upMin % 60);
  printRight(128, 55, buf);
}

// ---------- Home mode screen ----------
//  0..9    inverted bar: "HOME" + device number
//  13..44  device name (size 2 if it fits) + live state from Home Assistant
//  52..63  knob hints
static void drawHomeScreen() {
  char buf[24];
  oled.clearDisplay();
  oled.fillRect(0, 0, 128, 10, SSD1306_WHITE);
  printAt(2, 1, "HOME", 1, SSD1306_BLACK);
  const int n = homeSlotCount();
  if (n) snprintf(buf, sizeof(buf), "%d/%d", homeActiveSlot() + 1, n);
  else snprintf(buf, sizeof(buf), "no devices");
  printAt(126 - textWidth(buf, 1), 1, buf, 1, SSD1306_BLACK);

  const char *name = n ? homeSlotName(homeActiveSlot()) : "Not set up";
  if (textWidth(name, 2) <= 128) printCentered(15, name, 2);
  else printCentered(19, name, 1);
  homeStatusText(buf, sizeof(buf));
  printCentered(36, buf[0] ? buf : "...", 1);

  oled.drawFastHLine(0, 52, 128, SSD1306_WHITE);
  printAt(0, 55, "Push:on/off Turn:adj");
  oled.display();
}

// Redraws only when something visible changed. Keys/WPM refresh once a second so
// typing never stalls the scan loop on I2C (~23 ms per full frame).
void oledDrawStatusLine() {
  static bool forceFull = true;
  static bool lastConn = false, lastCaps = false, lastLowOff = false, lastShowBat = true;
  static int lastBat = -1;
  static unsigned long lastUpMin = (unsigned long)-1;
  static uint8_t lastFrame = 255;
  static bool toastShown = false;

  if (isMenuOpen()) { menuWasOpen = true; return; }
  if (homeModeActive()) {
    static uint32_t drawnStatus = 0, drawnPanel = 0;
    static int drawnSlot = -1;
    if (menuWasOpen || drawnStatus != homeStatusVersion() || drawnSlot != homeActiveSlot() ||
        drawnPanel != homePanelVersion()) {
      drawnStatus = homeStatusVersion(); drawnSlot = homeActiveSlot(); drawnPanel = homePanelVersion();
      drawHomeScreen();
    }
    menuWasOpen = false;
    forceFull = true;    // full home-screen redraw once Home mode ends
    return;
  }
  if (menuWasOpen) { menuWasOpen = false; forceFull = true; }

  const unsigned long now = millis();
  KbdSettings &s = getSettings();
  const bool conn    = bleKeyboard.isConnected();
  const bool caps    = isCapsFromHost();
  const int  bat     = getBatteryPercentage();
  const bool showBat = s.showBattery;
  const bool lowOff  = showBat && bat <= s.lowBatteryPct && (now / 500) % 2;
  const unsigned long upMin = now / 60000;
  const uint8_t frame = conn ? 0 : (uint8_t)((now / 350) % 4);
  const bool toastOn = now < toastUntil;

  bool changed = updateTypingStats(now) && conn;
  changed |= forceFull || toastDirty || toastOn != toastShown;
  changed |= conn != lastConn || caps != lastCaps || bat != lastBat || showBat != lastShowBat;
  changed |= lowOff != lastLowOff || upMin != lastUpMin || frame != lastFrame;
  if (!changed) return;

  forceFull = false; toastDirty = false; toastShown = toastOn;
  lastConn = conn; lastCaps = caps; lastBat = bat; lastShowBat = showBat;
  lastLowOff = lowOff; lastUpMin = upMin; lastFrame = frame;

  oled.clearDisplay();
  drawStatusBar(conn, bat, showBat, lowOff);
  if (conn) drawTypingBody(); else drawPairingBody(frame);
  drawFooter(caps, getKeyPressCount(), upMin);
  if (toastOn) drawToast();
  oled.display();
}

// Full-screen overlays (e.g. the update screen) call this to restore the home screen.
void oledForceRedraw() { menuWasOpen = true; }

// ---------- E-Ink runtime clock ----------
// Called from loop — only draws when 10-second block changes
static void drawEinkClockIfNeeded() {
  if (!einkInitialized) return;

  // Update every 10 seconds to avoid blocking the keyboard
  unsigned long nowSec = millis() / 1000;
  unsigned long block10 = nowSec / 10;
  if (block10 == lastDrawnSecond) return;
  lastDrawnSecond = block10;

  einkWake();
  einkDisplay.setRotation(0);
  einkDisplay.setFullWindow();
  einkDisplay.firstPage();
  do {
    einkDisplay.fillScreen(GxEPD_WHITE);
    einkDisplay.setTextSize(1);
    einkDisplay.setTextColor(GxEPD_BLACK);

    char buf[20];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
      nowSec / 3600, (nowSec % 3600) / 60, nowSec % 60);

    int16_t x1, y1;
    uint16_t w, h;
    einkDisplay.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    int x = (einkDisplay.width() - w) / 2;
    int y = (einkDisplay.height() - h) / 2 - y1;

    einkDisplay.setTextSize(2);
    einkDisplay.setCursor(x, y);
    einkDisplay.print(buf);

    einkDisplay.setTextSize(1);
    einkDisplay.setCursor(10, einkDisplay.height() - 10);
    einkDisplay.print(F("Runtime (HH:MM:SS)"));

  } while (einkDisplay.nextPage());
  // Don't sleep — keep awake for next 10s update in clock mode
}

// ---------- E-Ink status dashboard ----------
static void drawEinkStatusDashboard() {
  if (!einkInitialized) return;
  einkWake();
  einkDisplay.setRotation(0);
  einkDisplay.setFullWindow();
  einkDisplay.firstPage();
  do {
    einkDisplay.fillScreen(GxEPD_WHITE);
    einkDisplay.setTextSize(1);
    einkDisplay.setTextColor(GxEPD_BLACK);

    einkDisplay.setCursor(10, 20);
    einkDisplay.print(F("Keyboard Status"));
    einkDisplay.drawFastHLine(10, 26, 180, GxEPD_BLACK);

    int y = 40;
    char buf[40];

    snprintf(buf, sizeof(buf), "Battery: %d%%", getBatteryPercentage());
    einkDisplay.setCursor(10, y); einkDisplay.print(buf); y += 20;

    einkDisplay.setCursor(10, y);
    einkDisplay.print(bleKeyboard.isConnected() ? F("BLE: Connected") : F("BLE: Disconnected"));
    y += 20;

    einkDisplay.setCursor(10, y);
    einkDisplay.print(isCapsFromHost() ? F("CapsLock: ON") : F("CapsLock: OFF"));
    y += 20;

    unsigned long s = millis() / 1000;
    snprintf(buf, sizeof(buf), "Uptime: %02lu:%02lu:%02lu",
      s / 3600, (s % 3600) / 60, s % 60);
    einkDisplay.setCursor(10, y); einkDisplay.print(buf);

  } while (einkDisplay.nextPage());
  einkSleep();
}

// ---------- E-Ink idle screen ----------

// ---------- E-Ink Home panel ----------
// Device list for the Fn layer. One full refresh (~2 s) on entering Home mode
// or switching device; live state goes to the OLED, which is fast.
static void drawEinkHomePanel() {
  einkWake();
  einkDisplay.setRotation(0);
  einkDisplay.setFullWindow();
  const int n = homeSlotCount();
  const int active = homeActiveSlot();
  const uint8_t size = n <= 6 ? 2 : 1;
  const int rowH = size == 2 ? 22 : 16;
  einkDisplay.firstPage();
  do {
    einkDisplay.fillScreen(GxEPD_WHITE);
    einkDisplay.fillRect(0, 0, 200, 26, GxEPD_BLACK);
    einkDisplay.setTextColor(GxEPD_WHITE);
    einkDisplay.setTextSize(2);
    einkDisplay.setCursor(8, 6);
    einkDisplay.print(F("HOME"));

    einkDisplay.setTextSize(size);
    if (!n) {
      einkDisplay.setTextColor(GxEPD_BLACK);
      einkDisplay.setTextSize(1);
      einkDisplay.setCursor(8, 40);  einkDisplay.print(F("No devices set up."));
      einkDisplay.setCursor(8, 56);  einkDisplay.print(F("Hold the knob at power-on"));
      einkDisplay.setCursor(8, 68);  einkDisplay.print(F("and open 192.168.4.1"));
    }
    for (int i = 0; i < n; ++i) {
      const int y = 32 + i * rowH;
      char line[20];
      snprintf(line, sizeof(line), "%d %s", i + 1, homeSlotName(i));
      if (i == active) {
        einkDisplay.fillRect(0, y - 2, 200, rowH - 2, GxEPD_BLACK);
        einkDisplay.setTextColor(GxEPD_WHITE);
      } else {
        einkDisplay.setTextColor(GxEPD_BLACK);
      }
      einkDisplay.setCursor(6, y + (rowH - 2 - 8 * size) / 2 - 1);
      einkDisplay.print(line);
    }

    einkDisplay.setTextColor(GxEPD_BLACK);
    einkDisplay.setTextSize(1);
    einkDisplay.drawFastHLine(0, 174, 200, GxEPD_BLACK);
    einkDisplay.setCursor(4, 179);  einkDisplay.print(F("Knob: push=on/off turn=adj"));
    einkDisplay.setCursor(4, 190);  einkDisplay.print(F("Fn+1-9 pick  Fn/hold: exit"));
  } while (einkDisplay.nextPage());
  einkSleep();
}

void renderIdleScreen() {
  if (!einkInitialized) return;
  if (isMenuOpen() || otaScreenActive()) { einkAnimStop(); return; }

  // Home mode owns the panel; leaving it forces the idle screen to come back.
  static uint32_t drawnHomePanel = 0;
  static uint32_t seenHomePanel = 0;
  static unsigned long homePanelChangedAt = 0;
  if (homePanelVersion() != seenHomePanel) { seenHomePanel = homePanelVersion(); homePanelChangedAt = millis(); }
  if (homeModeActive()) {
    einkAnimStop();
    // wait a moment so a quick Fn+1, Fn+2 costs one refresh, not two
    if (drawnHomePanel != seenHomePanel && millis() - homePanelChangedAt > 250) {
      drawnHomePanel = seenHomePanel;
      Serial.println(F("[EINK] home panel"));
      drawEinkHomePanel();
      currentIdleMode = (MenuIdleMode)255;
      lastDrawnSecond = 0;
    }
    return;
  }
  drawnHomePanel = 0;

  MenuIdleMode mode = getSettings().einkIdleMode;
#if EINK_SHARES_ENCODER_PIN
  if (mode == IdleClock) mode = IdleTotoro;   // a redraw every 10 s would keep pausing the knob
#endif

  // Animation mode: one frame per call while idle (see EinkAnimation.cpp)
  if (mode == IdleAnimation) {
    if (currentIdleMode != IdleAnimation) {
      currentIdleMode = IdleAnimation;
      einkAnimInvalidate();
      Serial.println(F("[EINK] mode->Animation"));
    }
    einkAnimTick();
    return;
  }
  einkAnimStop();

  // Clock mode: update every 10 seconds, keep display awake
  if (mode == IdleClock) {
    if (currentIdleMode != IdleClock) {
      currentIdleMode = IdleClock;
      lastDrawnSecond = 0;
      Serial.println(F("[EINK] mode->Clock"));
    }
    unsigned long sinceKey = millis() - getLastKeyActivityTime();
    if (sinceKey < 3000 && getLastKeyActivityTime() > 0) return;
    drawEinkClockIfNeeded();
    return;
  }

  // Switching away from clock mode: sleep the display first
  if (currentIdleMode == IdleClock) {
    einkSleep();
    currentIdleMode = (MenuIdleMode)255;
    lastDrawnSecond = 0;
  }

  // Other modes: only redraw on mode change, and not while the keys or knob are in use
  // (the knob is paused during a refresh).
  unsigned long sinceKey = millis() - getLastKeyActivityTime();
  if (sinceKey < 3000 && getLastKeyActivityTime() > 0) return;
  if (millis() - getLastKnobActivityTime() < 3000 && getLastKnobActivityTime() > 0) return;
  if (mode == currentIdleMode) return;

  unsigned long now = millis();
  // Reset cooldown when mode actually changes so it renders immediately
  lastIdleRenderMs = now;
  currentIdleMode = mode;
  Serial.printf("[EINK] mode=%d\n", (int)mode);

  switch (mode) {
    case IdleTotoro: {
      einkWake();
      einkDisplay.setRotation(0);
      einkDisplay.setFullWindow();
      int x = (einkDisplay.width()  - totoro_200x200_1b_width)  / 2;
      int y = (einkDisplay.height() - totoro_200x200_1b_height) / 2;
      einkDisplay.firstPage();
      do {
        einkDisplay.fillScreen(GxEPD_WHITE);
        einkDisplay.drawBitmap(x, y, totoro_200x200_1b, totoro_200x200_1b_width, totoro_200x200_1b_height, GxEPD_BLACK);
      } while (einkDisplay.nextPage());
      einkSleep();
      break;
    }
    case IdleStatus:
      drawEinkStatusDashboard();
      break;
    default:
      break;
  }
}
