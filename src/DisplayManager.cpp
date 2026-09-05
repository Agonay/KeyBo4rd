#include "DisplayManager.h"
#include "BatteryIcons.h"
#include "BatteryManager.h"
#include "BluetoothHID.h"
#include "MenuManager.h"
#include "KeyboardMatrix.h"
#include <GxEPD2_BW.h>
#include "totoro_200x200_1b.h"
#include "ship16.h"
#include "fire16.h"

#define ENABLE_GxEPD2_GFX 0

// OLED
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
#define OLED_ADDR 0x3C

// E-Ink — WeAct 1.54" B&W (GDEH0154D67, SSD1681)
// Pinout matches WeAct Studio breakout board
#define CS_PIN   5
#define BUSY_PIN 4
#define RES_PIN  16
#define DC_PIN   17

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> einkDisplay(GxEPD2_154_D67(CS_PIN, DC_PIN, RES_PIN, BUSY_PIN));

// ---------- idle mode tracking ----------
static MenuIdleMode currentIdleMode = (MenuIdleMode)255; // sentinel: force first render
static bool einkInitialized = false;
static bool einkIsAwake = false;
static bool oledNeedsRedraw = true;
static bool menuWasOpen = false;
static uint8_t lastOledBrt = 100;
static bool lastCapsOn = false;
static int lastBatPct = -1;
static bool lastBleConn = false;

static unsigned long lastIdleRenderMs = 0;
static const unsigned long idleCooldownMs = 300000; // 5 min between full redraws (e-ink is slow)

// Clock state: only redraw when second changes
static unsigned long lastDrawnSecond = 0;
static bool clockNeedsUpdate = false;

static void einkWake() {
  if (!einkIsAwake && einkInitialized) {
    einkDisplay.init(115200, true, 50, false);
    einkIsAwake = true;
  }
}

static void einkSleep() {
  if (einkIsAwake) {
    einkDisplay.hibernate();
    einkIsAwake = false;
  }
}

void initDisplay() {
  delay(1000);

  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oled.ssd1306_command(0xD9); oled.ssd1306_command(0xF1);
  oled.ssd1306_command(SSD1306_SETDISPLAYCLOCKDIV);
  oled.ssd1306_command(0xF0);
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(WHITE);
  drawChargeTopLeft(42, 3);
  oled.drawFastHLine(0, 19, oled.width(), SSD1306_WHITE);
  oled.display();

  // E-Ink boot: show Totoro, then hibernate
  einkDisplay.init(115200, true, 50, false);
  einkDisplay.setRotation(0);
  einkDisplay.setFullWindow();
  int x = (einkDisplay.width()  - totoro_200x200_1b_width)  / 2;
  int y = (einkDisplay.height() - totoro_200x200_1b_height) / 2;
  einkDisplay.firstPage();
  do {
    einkDisplay.fillScreen(GxEPD_WHITE);
    einkDisplay.drawBitmap(x, y, totoro_200x200_1b, totoro_200x200_1b_width, totoro_200x200_1b_height, GxEPD_BLACK);
  } while (einkDisplay.nextPage());
  einkDisplay.hibernate();

  einkInitialized = true;
  einkIsAwake = false;
  currentIdleMode = (MenuIdleMode)255; // sentinel: force first renderIdleScreen call
  lastOledBrt     = getSettings().oledBrightness;
  lastDrawnSecond = 0;
}

// ---------- OLED helpers ----------
void applyOledBrightness(uint8_t percent) {
  uint8_t val = map(percent, 0, 100, 0, 255);
  oled.ssd1306_command(0x81); // set contrast
  oled.ssd1306_command(val);
}

static void oledClearLine() {
  oled.fillRect(0, 0, 128, 20, SSD1306_BLACK);
}

void oledDrawStatusLine() {
  bool nowOpen = isMenuOpen();
  if (nowOpen) { menuWasOpen = true; return; }
  if (menuWasOpen) { oledNeedsRedraw = true; lastBatPct = -1; lastCapsOn = !lastCapsOn; lastBleConn = !lastBleConn; menuWasOpen = false; }

  KbdSettings &s = getSettings();
  if (!s.showBattery && !oledNeedsRedraw) return;

  bool changed = false;
  bool capsOn = isCapsFromHost();
  bool bleConn = bleKeyboard.isConnected();
  int batPct   = getBatteryPercentage();

  if (capsOn != lastCapsOn) { lastCapsOn = capsOn; changed = true; }
  if (bleConn != lastBleConn) { lastBleConn = bleConn; changed = true; }
  if (batPct != lastBatPct && s.showBattery) { lastBatPct = batPct; changed = true; }

  if (!changed && !oledNeedsRedraw) return;
  oledNeedsRedraw = false;

  oledClearLine();

  // Caps lock icon
  if (!capsOn) {
    oled.drawBitmap(112, 1, ship16_bitmap, SHIP16_WIDTH, SHIP16_HEIGHT, SSD1306_WHITE);
  }

  // Battery
  if (s.showBattery) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%d%%", batPct);
    oled.setCursor(1, 1);
    oled.print(buf);
  }

  // BLE dot
  if (bleConn) {
    oled.fillCircle(1, 10, 2, SSD1306_WHITE);
  }

  oled.drawFastHLine(0, 19, 128, SSD1306_WHITE);
  oled.display();
}

void batteryStatus(int charge) {
  char buf[5];
  snprintf(buf, sizeof(buf), "%d%%", charge);
  int textSize = 1;
  oled.setTextSize(textSize);
  const int maxW = 6 * 3 * textSize, maxH = 8 * textSize;
  const int m = 1;
  const int clearX = oled.width() - m;
  oled.fillRect(clearX, 0, maxW + 2, maxH + 2, SSD1306_BLACK);
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  int x = 1;
  int y = 0 - y1;
  oled.setCursor(x, y);
  oled.print(buf);
  oled.display();
}

void capsLockStatus(bool on) {
  static bool prev = !on;
  if (on == prev) return;
  prev = on;
  const int w = SHIP16_WIDTH, h = SHIP16_HEIGHT;
  const int m = 1;
  const int x = oled.width() - w - m;
  const int y = m;
  oled.fillRect(x - 1, y - 1, w + 2, h + 2, SSD1306_BLACK);
  if (!on) {
    oled.drawBitmap(x, y, ship16_bitmap, w, h, SSD1306_WHITE);
  }
  oled.display();
}

void drawChargeTopLeft(uint8_t percent, uint8_t segments) {
  if (percent > 100) percent = 100;
  char buf[6];
  snprintf(buf, sizeof(buf), "%d%%", percent);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  int areaW = max<int>(w, 24);
  int areaH = h + 1 + 8;
  oled.fillRect(0, 0, areaW + 1, areaH, SSD1306_BLACK);
  int tx = 1;
  int ty = 0 - y1;
  oled.setCursor(tx, ty);
  oled.print(buf);
  int bx = 0;
  int by = ty + h + 1;
  drawBattery24x8(bx, by, segments);
}

void drawBattery24x8(int x, int y, uint8_t level) {
  if (level > 5) level = 5;
  const int H = 8;
  const int TIP_W = 3;
  const int BODY_W = 24 - TIP_W;
  const int BORDER = 1;
  oled.drawRect(x, y, BODY_W, H, SSD1306_WHITE);
  int tipY = y + (H - 4) / 2;
  oled.fillRect(x + BODY_W, tipY, TIP_W, 4, SSD1306_WHITE);
  const int innerX = x + BORDER;
  const int innerY = y + BORDER;
  const int innerW = BODY_W - 2 * BORDER;
  const int innerH = H - 2 * BORDER;
  const int SEG = 5;
  const int GAP = 1;
  const int segW = (innerW - (SEG - 1) * GAP) / SEG;
  for (int i = 0; i < level; ++i) {
    int sx = innerX + i * (segW + GAP);
    oled.fillRect(sx, innerY, segW, innerH, SSD1306_WHITE);
  }
}

void demoFourStatesRow(int y) {
  oled.fillRect(0, y, oled.width(), 10, SSD1306_BLACK);
  int x0 = 4;
  drawBattery24x8(x0 +  0, y, 0);
  drawBattery24x8(x0 + 30, y, 1);
  drawBattery24x8(x0 + 60, y, 3);
  drawBattery24x8(x0 + 90, y, 5);
}

// ---------- E-Ink runtime clock ----------
// Called from loop — only draws when second changes, wakes/sleeps display
static void drawEinkClockIfNeeded() {
  if (!einkInitialized) return;

  unsigned long nowSec = millis() / 1000;
  if (nowSec == lastDrawnSecond) return; // nothing changed
  lastDrawnSecond = nowSec;

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
  einkSleep();
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

static void drawRainIdleScreen() {
  if (!einkInitialized) return;
  einkWake();
  einkDisplay.setFullWindow();
  einkDisplay.firstPage();
  do {
    einkDisplay.fillScreen(GxEPD_WHITE);
    einkDisplay.setTextSize(1);
    einkDisplay.setTextColor(GxEPD_BLACK);

    int w = einkDisplay.width(), h = einkDisplay.height();
    einkDisplay.drawLine(0, h - 40, w, h - 40, GxEPD_BLACK);

    einkDisplay.fillRoundRect(40, 10, 120, 30, 10, GxEPD_BLACK);
    einkDisplay.fillRoundRect(60, 0, 80, 20, 10, GxEPD_BLACK);

    randomSeed(42);
    for (int i = 0; i < 40; i++) {
      int rx = random(10, w - 80);
      int ry = random(50, h - 50);
      int rl = random(6, 14);
      einkDisplay.drawLine(rx, ry, rx - 2, ry + rl, GxEPD_BLACK);
    }

    einkDisplay.setCursor(60, h - 25);
    einkDisplay.print(F("Rainy day"));
  } while (einkDisplay.nextPage());
}

void renderIdleScreen() {
  if (isMenuOpen()) return;
  if (!einkInitialized) return;

  MenuIdleMode mode = getSettings().einkIdleMode;

  // Clock mode: update every second, independent of cooldown
  if (mode == IdleClock) {
    if (currentIdleMode != IdleClock) {
      // Mode just changed to clock — force full redraw
      currentIdleMode = IdleClock;
      lastDrawnSecond = 0; // force immediate draw
    }
    // Don't update e-ink while user is typing (3s quiet window)
    unsigned long sinceKey = millis() - getLastKeyActivityTime();
    if (sinceKey < 3000 && getLastKeyActivityTime() > 0) return;
    drawEinkClockIfNeeded();
    return;
  }

  // Other modes: only redraw on mode change + cooldown
  if (isMenuOpen()) return;
  unsigned long sinceKey = millis() - getLastKeyActivityTime();
  if (sinceKey < 3000 && getLastKeyActivityTime() > 0) return;
  if (mode == currentIdleMode) return;

  unsigned long now = millis();
  if (now - lastIdleRenderMs < idleCooldownMs) return;

  currentIdleMode = mode;
  lastIdleRenderMs = now;

  // wake from hibernate, draw, sleep again
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
    case IdleAnimation:
      drawRainIdleScreen();
      einkSleep();
      break;
    default:
      break;
  }
}

void menuDemo() {
  oled.fillRect(0, 20, 128, 44, SSD1306_BLACK);
  oled.fillRect(0, 21, 128, 8, SSD1306_WHITE);
  oled.fillRect(0, 31, 128, 8, SSD1306_WHITE);
  oled.fillRect(0, 41, 128, 8, SSD1306_WHITE);
  oled.fillRect(0, 51, 128, 8, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_BLACK);
  char *text = (char *)"Prueba menu";
  oled.setCursor(1, 21);
  oled.print(text);
}
