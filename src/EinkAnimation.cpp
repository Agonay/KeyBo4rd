#include "EinkAnimation.h"
#include "DisplayManager.h"
#include "KeyboardMatrix.h"
#include "RotaryEncoder.h"

// ---------------------------------------------------------------------------
// A grumpy storm cloud rains on a cat under an umbrella. The acts:
//   1 FULL     full refresh per frame: the panel flashes black/white, ~2 s each
//   2 PARTIAL  partial refresh of the whole screen: no flash, ~0.3-0.4 s each,
//              ghosting slowly builds up
//   3 WINDOW   partial refresh of the bottom strip only: everything above the
//              dotted line freezes mid-rain. The refresh takes about as long as
//              act 2: the waveform sets the time, not the area.
//   4 FLASH    full refresh of an inverted frame with a bolt; the full-refresh
//              flicker is the lightning
//   5 CLEAN    full refresh of the normal scene, wiping the ghosting
// then back to act 2.
//
// The panel stays powered (no hibernate) between frames, but SPI is released
// after each one so GPIO18 is the knob's CLK again during the gap.
// ---------------------------------------------------------------------------

enum Act : uint8_t { ACT_FULL, ACT_PARTIAL, ACT_WINDOW, ACT_FLASH, ACT_CLEAN };

static const char *ACT_LABEL[] = { "1 FULL", "2 PARTIAL", "3 WINDOW", "4 FLASH", "5 CLEAN" };

static const unsigned long IDLE_BEFORE_START_MS = 5000;
static const unsigned long FRAME_GAP_MS         = 150;              // knob is live in the gap
static const unsigned long MAX_RUN_MS           = 3UL * 60UL * 1000UL;

static const int W = 200;
static const int SKY_TOP = 46;      // cloud bottom: rain starts here
static const int GROUND = 160;
static const int HUD_Y = 188;
static const int WIN_Y = 136;       // act 3 window: y = 136..199 (includes the HUD)
static const int UMB_X = 150, UMB_Y = 124, UMB_R = 24;
static const int DROPS = 30;

static bool running = false;
static bool introDone = false;       // the 3-frame full-refresh intro plays once per selection
static bool capped = false;
static unsigned long cappedAt = 0;
static unsigned long runStart = 0;
static unsigned long lastFrameEnd = 0;
static unsigned long lastRefreshMs = 0;
static uint32_t frame = 0;
static uint32_t strikes = 0;
static Act act = ACT_FULL;
static int actFrames = 0;
static unsigned long actMsSum = 0;

static uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU;
  x ^= x >> 15; x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

static unsigned long lastActivity() {
  return max(getLastKeyActivityTime(), getLastKnobActivityTime());
}

static int cloudX(uint32_t f) { return 40 + (int)(38.0f * sinf(f * 0.07f)); }

// y of the umbrella's top surface at column x, or -1 if x is outside it
static int umbrellaTop(int x) {
  int dx = x - UMB_X;
  if (dx < -UMB_R || dx > UMB_R) return -1;
  return UMB_Y - (int)sqrtf((float)(UMB_R * UMB_R - dx * dx));
}

// ---------- scene pieces ----------
static void drawCloud(int x, uint16_t fg, uint16_t bg, bool angry) {
  auto &d = einkDisplay;
  d.fillCircle(x + 20, 30, 15, fg);
  d.fillCircle(x + 46, 21, 21, fg);
  d.fillCircle(x + 76, 25, 18, fg);
  d.fillCircle(x + 98, 33, 12, fg);
  d.fillRoundRect(x + 6, 28, 102, SKY_TOP - 28, 9, fg);
  // face: eyes look down at the cat, brows frown
  const int ey = 31;
  d.fillCircle(x + 44, ey, 4, bg);
  d.fillCircle(x + 66, ey, 4, bg);
  d.fillCircle(x + 45, ey + 2, 2, fg);
  d.fillCircle(x + 67, ey + 2, 2, fg);
  const int lift = angry ? 3 : 0;
  d.drawLine(x + 38, ey - 8 - lift, x + 49, ey - 5, bg);
  d.drawLine(x + 38, ey - 7 - lift, x + 49, ey - 4, bg);
  d.drawLine(x + 72, ey - 8 - lift, x + 61, ey - 5, bg);
  d.drawLine(x + 72, ey - 7 - lift, x + 61, ey - 4, bg);
  // mouth: a small frown
  d.drawLine(x + 50, ey + 9, x + 55, ey + 7, bg);
  d.drawLine(x + 55, ey + 7, x + 60, ey + 9, bg);
}

static void drawUmbrellaAndCat(uint32_t f, uint16_t fg, uint16_t bg) {
  auto &d = einkDisplay;
  // canopy: half disc with a scalloped edge
  d.fillCircle(UMB_X, UMB_Y, UMB_R, fg);
  d.fillRect(UMB_X - UMB_R, UMB_Y + 1, 2 * UMB_R + 1, UMB_R, bg);
  for (int i = 0; i < 4; ++i) d.fillCircle(UMB_X - UMB_R + 6 + i * 12, UMB_Y + 2, 6, bg);
  d.drawFastVLine(UMB_X, UMB_Y - UMB_R - 4, 4, fg);                // tip
  for (int i = -2; i <= 2; i += 2) d.drawLine(UMB_X, UMB_Y - UMB_R + 2, UMB_X + i * 9, UMB_Y - 2, bg); // ribs
  // handle with a J hook
  d.drawFastVLine(UMB_X, UMB_Y, GROUND - 6 - UMB_Y, fg);
  d.drawFastVLine(UMB_X + 1, UMB_Y, GROUND - 6 - UMB_Y, fg);
  d.drawLine(UMB_X, GROUND - 6, UMB_X - 3, GROUND - 3, fg);
  d.drawLine(UMB_X - 3, GROUND - 3, UMB_X - 6, GROUND - 5, fg);

  // cat sitting to the right of the handle
  const int cx = UMB_X + 11;
  d.fillRoundRect(cx - 6, GROUND - 14, 14, 14, 6, fg);             // body
  d.fillCircle(cx, GROUND - 19, 6, fg);                             // head
  d.fillTriangle(cx - 6, GROUND - 21, cx - 5, GROUND - 28, cx - 2, GROUND - 24, fg);
  d.fillTriangle(cx + 2, GROUND - 24, cx + 5, GROUND - 28, cx + 6, GROUND - 21, fg);
  if (f % 14 == 0) {                                                // blink
    d.drawFastHLine(cx - 3, GROUND - 19, 2, bg);
    d.drawFastHLine(cx + 2, GROUND - 19, 2, bg);
  } else {
    d.fillRect(cx - 3, GROUND - 21, 2, 3, bg);
    d.fillRect(cx + 2, GROUND - 21, 2, 3, bg);
  }
  // tail swishes
  const int sw = (int)(5.0f * sinf(f * 0.9f));
  for (int t = 0; t < 2; ++t) {
    d.drawLine(cx + 7, GROUND - 3 + t, cx + 13, GROUND - 6 + t, fg);
    d.drawLine(cx + 13, GROUND - 6 + t, cx + 15 + sw / 2, GROUND - 14 + sw + t, fg);
  }
}

static void drawGround(uint16_t fg) {
  auto &d = einkDisplay;
  d.drawFastHLine(0, GROUND, W, fg);
  d.drawFastHLine(0, GROUND + 1, W, fg);
  // puddle shimmer
  static const uint8_t px[] = { 8, 40, 71, 104, 180, 22, 58, 92, 128, 170 };
  for (int i = 0; i < 10; ++i) d.drawFastHLine(px[i], GROUND + 6 + (i % 5) * 4, 10 + (i * 7) % 12, fg);
}

static void drawRain(uint32_t f, uint16_t fg) {
  auto &d = einkDisplay;
  const int span = GROUND - SKY_TOP;
  const int cx = cloudX(f);
  for (int i = 0; i < DROPS; ++i) {
    const uint32_t h = hash32(i * 2654435761U + 17);
    const int off   = h % 100;
    const int speed = 10 + (h >> 8) % 8;
    const int len   = 7 + (h >> 12) % 8;
    const int p     = (int)((h >> 16) % span) + (int)f * speed;
    const int y     = SKY_TOP + p % span;
    const int x     = cx + 8 + off - (y - SKY_TOP) / 5;               // wind blows the rain left

    // splash + ripple where this drop last landed
    const int since = (p % span) / speed;                              // frames since landing
    const int lx = cx + 8 + off - span / 5;
    const int ut = umbrellaTop(lx);
    if (ut < 0 && since < 4 && f >= (uint32_t)since) {
      const int ry = GROUND + 7 + (int)((h >> 20) % 18);
      d.drawEllipse(lx, ry, 3 + since * 4, 1 + since, fg);
      if (since == 0) {                                                // crown
        d.drawLine(lx - 2, GROUND - 1, lx - 4, GROUND - 5, fg);
        d.drawLine(lx + 2, GROUND - 1, lx + 4, GROUND - 5, fg);
      }
    }

    // falling streak, stopped by the umbrella
    const int top = umbrellaTop(x);
    if (top >= 0 && y > top) {
      if (y - top < speed) {                                           // just hit: bounce droplets
        d.drawPixel(x - 3, top - 3, fg);
        d.drawPixel(x + 3, top - 4, fg);
        d.drawPixel(x - 5, top - 1, fg);
      }
      continue;
    }
    const int y0 = max(SKY_TOP, y - len);
    const int dx = (y - y0) / 5;
    d.drawLine(x, y, x + dx, y0, fg);
  }
}

static void drawBolt(uint32_t seed, int x, uint16_t fg) {
  auto &d = einkDisplay;
  int px = x, py = SKY_TOP - 2;
  int seg = 0;
  while (py < GROUND) {
    const uint32_t h = hash32(seed * 977 + seg);
    const int nx = constrain(px + (int)(h % 25) - 12, 4, W - 4);
    const int ny = min(GROUND, py + 10 + (int)((h >> 8) % 10));
    for (int t = -1; t <= 1; ++t) d.drawLine(px + t, py, nx + t, ny, fg);
    if (seg == 2 || seg == 5) {                                        // branch
      const int bx = nx + ((h >> 16) & 1 ? 18 : -18), by = ny + 16;
      d.drawLine(nx, ny, bx, by, fg);
      d.drawLine(bx, by, bx + 4, by + 9, fg);
    }
    px = nx; py = ny; ++seg;
  }
  d.fillCircle(px, GROUND, 3, fg);                                     // strike point
}

static void drawHud(uint16_t fg, uint16_t bg) {
  auto &d = einkDisplay;
  d.fillRect(0, HUD_Y, W, W - HUD_Y, fg);
  d.setTextSize(1);
  d.setTextColor(bg);
  d.setCursor(3, HUD_Y + 2);
  d.print(ACT_LABEL[act]);
  char buf[20];
  snprintf(buf, sizeof(buf), "#%lu", (unsigned long)frame);
  d.setCursor(78, HUD_Y + 2);
  d.print(buf);
  if (lastRefreshMs) snprintf(buf, sizeof(buf), "%lums", lastRefreshMs);
  else               snprintf(buf, sizeof(buf), "--ms");
  d.setCursor(W - 3 - (int)strlen(buf) * 6, HUD_Y + 2);
  d.print(buf);
}

static void drawScene() {
  auto &d = einkDisplay;
  const bool flash = act == ACT_FLASH;
  const uint16_t fg = flash ? GxEPD_WHITE : GxEPD_BLACK;
  const uint16_t bg = flash ? GxEPD_BLACK : GxEPD_WHITE;
  d.fillScreen(bg);
  drawRain(frame, fg);
  drawGround(fg);
  drawCloud(cloudX(frame), fg, bg, flash);
  if (flash) drawBolt(strikes, cloudX(frame) + 30 + (int)(hash32(strikes) % 50), fg);
  drawUmbrellaAndCat(frame, fg, bg);
  if (act == ACT_WINDOW)                                               // window edge
    for (int x = 0; x < W; x += 4) d.drawFastHLine(x, WIN_Y, 2, fg);
  drawHud(fg, bg);
}

// ---------- frame + act sequencing ----------
static void renderFrame() {
  einkAcquireBus();
  switch (act) {
    case ACT_PARTIAL: einkDisplay.setPartialWindow(0, 0, W, W); break;
    case ACT_WINDOW:  einkDisplay.setPartialWindow(0, WIN_Y, W, W - WIN_Y); break;
    default:          einkDisplay.setFullWindow(); break;
  }
  const unsigned long t0 = millis();
  einkDisplay.firstPage();
  do { drawScene(); } while (einkDisplay.nextPage());
  lastRefreshMs = millis() - t0;
  einkReleaseBus();
  lastFrameEnd = millis();
  actMsSum += lastRefreshMs;
  ++actFrames;
  ++frame;
}

static int actLength(Act a) {
  switch (a) {
    case ACT_FULL:    return introDone ? 1 : 3;
    case ACT_PARTIAL: return 25;
    case ACT_WINDOW:  return 20;
    default:          return 1;
  }
}

static void nextAct() {
  Serial.printf("[EINK] %s: %d frames, avg %lu ms\n", ACT_LABEL[act], actFrames,
                actFrames ? actMsSum / actFrames : 0);
  if (act == ACT_FULL) introDone = true;
  switch (act) {
    case ACT_FULL:    act = ACT_PARTIAL; break;
    case ACT_PARTIAL: act = ACT_WINDOW; break;
    case ACT_WINDOW:  act = ACT_FLASH; ++strikes; break;
    case ACT_FLASH:   act = ACT_CLEAN; break;
    case ACT_CLEAN:   act = ACT_PARTIAL; break;
  }
  actFrames = 0;
  actMsSum = 0;
}

// ---------- public ----------
bool einkAnimRunning() { return running; }

void einkAnimReset() {
  einkAnimStop();
  introDone = false;
  capped = false;
}

void einkAnimStop() {
  if (!running) return;
  running = false;
  einkSleep();
  Serial.println(F("[EINK] animation stopped"));
}

void einkAnimTick() {
  if (!einkReady()) return;
  const unsigned long now = millis();
  const unsigned long activity = lastActivity();

  if (!running) {
    if (activity && now - activity < IDLE_BEFORE_START_MS) return;
    if (!activity && now < IDLE_BEFORE_START_MS) return;
    if (capped && activity <= cappedAt) return;      // wait for the user before running again
    capped = false;
    running = true;
    runStart = now;
    act = ACT_FULL;                                   // first refresh after a wake is full anyway
    actFrames = 0;
    actMsSum = 0;
    einkWake();
    einkDisplay.setRotation(0);
    Serial.println(F("[EINK] animation start"));
  }

  if (activity > runStart) { einkAnimStop(); return; }
  if (now - lastFrameEnd < FRAME_GAP_MS) return;

  if (now - runStart > MAX_RUN_MS) {                  // panel care: end on a clean full refresh
    act = ACT_CLEAN;
    renderFrame();
    capped = true;
    cappedAt = millis();
    Serial.println(F("[EINK] animation time cap"));
    einkAnimStop();
    return;
  }

  renderFrame();
  if (actFrames >= actLength(act)) nextAct();
}
