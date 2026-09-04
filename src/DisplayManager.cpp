#include "DisplayManager.h"
#include "BatteryIcons.h"
#include "BatteryManager.h"
#include "BluetoothHID.h"
#include "MenuManager.h"
#include <GxEPD2_BW.h>
#include "totoro_200x200_1b.h"
#include "ship16.h"
#include "fire16.h"

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
static MenuIdleMode currentIdleMode = IdleTotoro; // sentinel: force first render
static bool oledNeedsRedraw = true;
static uint8_t lastOledBrt = 100;
static bool lastCapsOn = false;
static int lastBatPct = -1;
static bool lastBleConn = false;

static unsigned long lastIdleRenderMs = 0;
static const unsigned long idleCooldownMs = 120000; // 2 min between full redraws

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

  // E-Ink boot: show Totoro
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

  currentIdleMode = IdleTotoro;
  lastOledBrt     = getSettings().oledBrightness;
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

// ---------- E-Ink idle screen ----------
static inline int roundDown8(int v) { return v & ~7; }
static inline int roundUp8(int v)   { return (v + 7) & ~7; }

static void drawBackground() {
  einkDisplay.fillScreen(GxEPD_WHITE);
  int w = einkDisplay.width(), h = einkDisplay.height();
  einkDisplay.drawLine(0, h - 40, w, h - 40, GxEPD_BLACK);
  einkDisplay.fillRect(w - 70, 0, 70, h, GxEPD_BLACK);
  einkDisplay.fillCircle(w - 70, h - 40, 60, GxEPD_WHITE);
  for (int i = 0; i < 8; i++) {
    int x = w / 2 - 20 + i * 5;
    einkDisplay.fillRect(x, h - 28 + (i % 2), 4, 3, GxEPD_BLACK);
  }
}

struct Drop { int x, y, len; };
static void stepRain(Drop *drops, int count, int w, int h) {
  for (int i = 0; i < count; i++) {
    drops[i].y += 5;
    drops[i].x -= 1;
    if (drops[i].y > h) {
      drops[i].y = -(random(10, 50));
      drops[i].x = random(0, w);
      drops[i].len = random(6, 12);
    }
  }
}

static void drawRainPartial(Drop *drops, int count) {
  const int w = einkDisplay.width(), h = einkDisplay.height();
  int minx = w, miny = h, maxx = 0, maxy = 0;
  for (int i = 0; i < count; i++) {
    int x0 = drops[i].x, y0 = drops[i].y;
    int x1 = x0 - 2, y1 = y0 + drops[i].len;
    if (x0 < minx) minx = x0; if (x1 < minx) minx = x1;
    if (y0 < miny) miny = y0; if (y1 < miny) miny = y1;
    if (x0 > maxx) maxx = x0; if (x1 > maxx) maxx = x1;
    if (y0 > maxy) maxy = y0; if (y1 > maxy) maxy = y1;
  }
  minx = max(0, roundDown8(minx - 2));
  maxx = min(w, roundUp8(maxx + 2));
  miny = max(0, miny - 2);
  maxy = min(h, maxy + 2);
  int ww = maxx - minx;
  int hh = maxy - miny;
  if (ww <= 0 || hh <= 0) return;
  einkDisplay.setPartialWindow(minx, miny, ww, hh);
  einkDisplay.firstPage();
  do {
    einkDisplay.fillRect(minx, miny, ww, hh, GxEPD_WHITE);
    for (int i = 0; i < count; i++) {
      int x0 = drops[i].x, y0 = drops[i].y;
      int x1 = x0 - 2, y1 = y0 + drops[i].len;
      einkDisplay.drawLine(x0, y0, x1, y1, GxEPD_BLACK);
    }
  } while (einkDisplay.nextPage());
}

static void drawFireFrame(int x, int y, int frameIndex) {
  const int fw = FIRE_W, fh = FIRE_H;
  int bx = x - fw / 2;
  int by = y - fh;
  int winX = roundDown8(bx - 1);
  int winW = roundUp8(bx + fw + 1) - winX;
  int winY = by - 1;
  int winH = fh + 2;
  einkDisplay.setPartialWindow(winX, winY, winW, winH);
  einkDisplay.firstPage();
  do {
    einkDisplay.fillRect(bx - 1, by - 1, fw + 2, fh + 2, GxEPD_WHITE);
    const uint8_t *bmp = (const uint8_t *)pgm_read_ptr(&FIRE_FRAMES[frameIndex]);
    einkDisplay.drawBitmap(bx, by, bmp, fw, fh, GxEPD_BLACK);
  } while (einkDisplay.nextPage());
}

void einkAnimateCampfireRain(uint32_t duration_ms) {
  const bool fast = einkDisplay.epd2.hasFastPartialUpdate;
  einkDisplay.setFullWindow();
  einkDisplay.firstPage(); do { drawBackground(); } while (einkDisplay.nextPage());

  const int W = einkDisplay.width(), H = einkDisplay.height();
  const int N_DROPS = 18;
  Drop drops[N_DROPS];
  for (int i = 0; i < N_DROPS; i++) {
    drops[i].x = random(0, W);
    drops[i].y = random(-H, 0);
    drops[i].len = random(6, 12);
  }
  const int fireX = W / 2;
  const int fireY = H - 30;

  uint32_t endMs = millis() + duration_ms;
  int frame = 0, framesSinceClean = 0;
  const int CLEAN_EVERY = 40;

  while ((int32_t)(millis() - endMs) < 0) {
    stepRain(drops, N_DROPS, W, H);
    drawRainPartial(drops, N_DROPS);
    drawFireFrame(fireX, fireY, frame);
    frame = (frame + 1) % FIRE_NFRAMES;
    if (++framesSinceClean >= CLEAN_EVERY) {
      framesSinceClean = 0;
      einkDisplay.setFullWindow();
      einkDisplay.firstPage(); do { drawBackground(); } while (einkDisplay.nextPage());
    }
    delay(fast ? 120 : 250);
  }
  einkDisplay.hibernate();
}

// ---------- E-Ink status dashboard ----------
void drawEinkStatusDashboard() {
  einkDisplay.init(115200, true, 50, false);
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
  einkDisplay.hibernate();
}

static void drawRainIdleScreen() {
  einkDisplay.setRotation(0);
  einkDisplay.setFullWindow();
  einkDisplay.firstPage();
  do {
    einkDisplay.fillScreen(GxEPD_WHITE);
    einkDisplay.setTextSize(1);
    einkDisplay.setTextColor(GxEPD_BLACK);
    einkDisplay.setCursor(10, 100);
    einkDisplay.print(F("Rainy day"));
  } while (einkDisplay.nextPage());
}

void renderIdleScreen() {
  MenuIdleMode mode = getSettings().einkIdleMode;
  if (mode == currentIdleMode) return;
  unsigned long now = millis();
  if (now - lastIdleRenderMs < idleCooldownMs) return;

  currentIdleMode = mode;
  lastIdleRenderMs = now;

  einkDisplay.init(115200, true, 50, false);
  switch (mode) {
    case IdleTotoro: {
      einkDisplay.setRotation(0);
      einkDisplay.setFullWindow();
      int x = (einkDisplay.width()  - totoro_200x200_1b_width)  / 2;
      int y = (einkDisplay.height() - totoro_200x200_1b_height) / 2;
      einkDisplay.firstPage();
      do {
        einkDisplay.fillScreen(GxEPD_WHITE);
        einkDisplay.drawBitmap(x, y, totoro_200x200_1b, totoro_200x200_1b_width, totoro_200x200_1b_height, GxEPD_BLACK);
      } while (einkDisplay.nextPage());
      break;
    }
    case IdleStatus:
      drawEinkStatusDashboard();
      return;
    case IdleAnimation:
      drawRainIdleScreen();
      break;
    default:
      break;
  }
  einkDisplay.hibernate();
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
