#include "EinkAnimation.h"
#include "EinkScene.h"
#include "DisplayManager.h"
#include "KeyboardMatrix.h"
#include "RotaryEncoder.h"

// ---------------------------------------------------------------------------
// Rainy street (EinkScene.h, made by tools/make_eink_scene.py) with small
// changes per frame: lamp glows that flicker with the rain, fog drifting over
// the hills, ripples in the puddles, rain streaks and the cat in the window.
//
// Smoothness comes from changing few pixels per frame and never doing a full
// refresh while you're around:
//   * light effects only flip pixels of the base picture, in blue-noise order
//     (EINK_BLUE): at zero effect the picture is exactly the base, and a small
//     change only flips a few pixels where it happens, so nothing crawls.
//   * pausing (key/knob) only powers the panel off; resuming continues with
//     partial refreshes. If the panel was put to deep sleep (menu, mode
//     change), the last frame is written back into the controller's "previous"
//     RAM so the next partial refresh is still clean.
//   * a full refresh happens only when the scene first appears, and once after
//     30 min of you being away (to clear ghosting).
// SPI is released after every frame so GPIO18 is the knob's CLK in the gaps.
// ---------------------------------------------------------------------------

static const int W = 200, H = 200, ROW = W / 8;
static const unsigned long IDLE_BEFORE_START_MS = 5000;
static const unsigned long FRAME_GAP_MS         = 150;
static const unsigned long MAX_RUN_MS           = 30UL * 60UL * 1000UL;

enum PanelState : uint8_t {
  PANEL_UNKNOWN,   // something else is on screen: next frame needs a full refresh
  PANEL_AWAKE,     // controller initialised, both RAMs hold `shown`
  PANEL_ASLEEP,    // deep sleep, the screen shows `shown` (controller RAM lost)
};

static uint8_t buf[W * H / 8];
static uint8_t shown[W * H / 8];
static PanelState panel = PANEL_UNKNOWN;
static bool running = false;
static bool capped = false;
static unsigned long cappedAt = 0;
static unsigned long runStart = 0;
static unsigned long lastFrameEnd = 0;
static uint32_t frame = 0;
static uint32_t msSum = 0, msFrames = 0;

// ---------- helpers ----------
static uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU;
  x ^= x >> 15; x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

static unsigned long lastActivity() {
  return max(getLastKeyActivityTime(), getLastKnobActivityTime());
}

static inline bool basePx(int x, int y) {
  return pgm_read_byte(&EINK_SCENE[y * ROW + (x >> 3)]) & (0x80 >> (x & 7));
}
static inline bool px(int x, int y) { return buf[y * ROW + (x >> 3)] & (0x80 >> (x & 7)); }
static inline void setPx(int x, int y, bool white) {
  if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
  uint8_t &b = buf[y * ROW + (x >> 3)];
  const uint8_t m = 0x80 >> (x & 7);
  if (white) b |= m; else b &= ~m;
}
static inline uint8_t blue(int x, int y) { return pgm_read_byte(&EINK_BLUE[((y & 63) << 6) | (x & 63)]); }

// Brighten by `d` (0..255): black pixels turn white in blue-noise order.
static inline void lighten(int x, int y, int d) {
  if (d > 0 && !px(x, y) && blue(x, y) < d) setPx(x, y, true);
}

// Draw marks in the colour that stands out against the base picture there.
static bool contrastColour(int x, int y) {
  int white = 0, n = 0;
  for (int dy = -2; dy <= 2; ++dy)
    for (int dx = -2; dx <= 2; ++dx) {
      const int xx = x + dx, yy = y + dy;
      if ((unsigned)xx >= (unsigned)W || (unsigned)yy >= (unsigned)H) continue;
      white += basePx(xx, yy); ++n;
    }
  return white * 100 < n * 55;   // mostly dark around -> draw white
}

// Lamps burn steadily with a slow breathing; now and then the rain makes one dip.
static float flicker(float t, uint32_t seed) {
  if (hash32((uint32_t)(t * 1.5f) * 31 + seed) % 13 == 0) return 0.35f;
  return 0.85f + 0.15f * sinf(t * 0.7f + seed);
}

// ---------- scene layers ----------
static void drawLights(float t) {
  // wall lamp: cone of light on the wall under the shade
  const float f1 = flicker(t, 1);
  for (int y = 38; y <= 70; ++y)
    for (int x = 9; x <= 61; ++x) {
      const float dx = (x - 35) / 26.0f, dy = (y - 40) / 30.0f, r = dx * dx + dy * dy;
      if (r < 1) lighten(x, y, (int)(150 * f1 * (1 - r) * (1 - r)));
    }
  // street lamp head
  const float f2 = flicker(t, 2);
  for (int y = 62; y <= 86; ++y)
    for (int x = 102; x <= 126; ++x) {
      const float r = ((x - 114) * (x - 114) + (y - 74) * (y - 74)) / 144.0f;
      if (r < 1) lighten(x, y, (int)(130 * f2 * (1 - r) * (1 - r)));
    }
  // paper lantern halo
  const float f3 = flicker(t, 3);
  for (int y = 57; y <= 105; ++y)
    for (int x = 160; x < W; ++x) {
      const float rr = sqrtf((float)((x - 184) * (x - 184) + (y - 81) * (y - 81)));
      if (rr >= 8 && rr < 24) {
        const float k = 1 - (rr - 8) / 16;
        lighten(x, y, (int)(120 * f3 * k * k));
      }
    }
  // fog drifting over the hills and down the far street
  for (int y = 24; y <= 110; ++y) {
    const float wob = 0.6f * sinf(0.13f * y + 0.1f * t);
    const float fy = min(1.0f, (y - 24) / 10.0f) * min(1.0f, (110 - y) / 25.0f);
    for (int x = 96; x <= 176; ++x) {
      const float n = sinf(0.09f * x - 0.21f * t + wob)
                    + 0.6f * sinf(0.17f * x + 0.05f * y - 0.33f * t)
                    + 0.4f * sinf(0.05f * x - 0.07f * y - 0.11f * t);
      if (n <= 0.2f) continue;
      const float fx = min(1.0f, (x - 96) / 12.0f) * min(1.0f, (176 - x) / 12.0f);
      lighten(x, y, (int)(80 * (n - 0.2f) * fx * fy));
    }
  }
}

// The cat in the bookshop window: tail swishes, an ear twitches, and now and
// then it turns its head to you and blinks.
static const char *const CAT_BODY[] = {
  "..#...#.", "..##.##.", "..#####.", "..#####.", "...###..", "..#####.", ".######.",
  ".######.", ".#######", ".#######", "########", "########", "########",
};
static const int8_t CAT_TAIL[4][6][2] = {
  {{0, 9}, {0, 10}, {0, 11}, {0, 12}, {-1, -1}, {-1, -1}},   // down
  {{0, 7}, {0, 8}, {0, 9}, {0, 10}, {1, 6}, {-1, -1}},       // half up
  {{0, 5}, {0, 6}, {0, 7}, {0, 8}, {1, 4}, {2, 4}},          // curled up
  {{0, 7}, {0, 8}, {0, 9}, {1, 6}, {2, 5}, {-1, -1}},
};
static const uint8_t CAT_TAIL_SEQ[16] = {0, 0, 0, 0, 1, 2, 2, 3, 2, 1, 0, 0, 0, 0, 0, 0};
static const int CAT_X = 19, CAT_Y = 110;

static void drawCat(uint32_t f) {
  for (int j = 0; j < 13; ++j)
    for (int i = 0; i < 8; ++i) setPx(CAT_X + i, CAT_Y + j, CAT_BODY[j][i] != '#');
  const auto &tail = CAT_TAIL[CAT_TAIL_SEQ[(f / 2) % 16]];
  for (int k = 0; k < 6 && tail[k][0] >= 0; ++k) setPx(CAT_X + tail[k][0], CAT_Y + tail[k][1], false);
  const uint32_t p = f % 40;
  if (p >= 24 && p < 36 && p != 30) {           // looking at you (blinks at 30)
    setPx(CAT_X + 3, CAT_Y + 2, true);
    setPx(CAT_X + 5, CAT_Y + 2, true);
  }
  if (f % 23 == 5) { setPx(CAT_X + 6, CAT_Y, true); setPx(CAT_X + 6, CAT_Y + 1, false); }   // ear twitch
}

// Ripples spread and fade in the puddles on the street.
static void drawRipples(uint32_t f) {
  for (int i = 0; i < 9; ++i) {
    const uint32_t per = 6 + hash32(i * 7 + 1) % 4;
    const uint32_t off = hash32(i * 13 + 5);
    const uint32_t age = (f + off) % per, cyc = (f + off) / per;
    if (age >= 5) continue;
    const uint32_t h = hash32(i * 977 + cyc * 131);
    const int y = 124 + h % 74;
    const float s = (y - 120) / 79.0f;
    const int xl = (int)(112 - 32 * s), xr = (int)(150 + 25 * s);   // street edges
    const int x = xl + 4 + (int)((h >> 8) % max(1, xr - xl - 8));
    const float rx = 1.5f + age * 2.0f, ry = max(1.0f, rx * 0.33f);
    const bool col = contrastColour(x, y);
    const int steps = (int)(rx * 6) + 8;
    for (int k = 0; k < steps; ++k) {
      if (age >= 3 && (k & 1)) continue;         // fading: dotted ring
      const float a = 6.2831853f * k / steps;
      setPx((int)lroundf(x + rx * cosf(a)), (int)lroundf(y + ry * sinf(a)), col);
    }
  }
}

static void drawRain(uint32_t f) {
  for (int i = 0; i < 42; ++i) {
    const uint32_t h = hash32(i * 2654435761U + 17);
    const int x0 = (int)(h % 230) - 15;
    const int speed = 16 + (h >> 8) % 10;
    const int len = 4 + (h >> 12) % 5;
    const int yb = (int)(((h >> 16) % 240 + f * speed) % 240) - 20;
    for (int k = 0; k < len; ++k) {
      const int y = yb - k, x = x0 + (yb - k) / 7;
      if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) setPx(x, y, contrastColour(x, y));
    }
  }
}

static void renderScene(uint32_t f) {
  memcpy_P(buf, EINK_SCENE, sizeof(buf));
  const float t = f * 0.4f;     // ~0.4 s per frame on the panel
  drawLights(t);
  drawCat(f);
  drawRipples(f);
  drawRain(f);
}

// ---------- panel I/O ----------
static void sendFrame(bool full) {
  auto &epd = einkDisplay.epd2;
  einkAcquireBus();
  const unsigned long t0 = millis();
  if (full) {
    epd.writeImageForFullRefresh(buf, 0, 0, W, H);
    epd.refresh(false);
  } else {
    epd.writeImage(buf, 0, 0, W, H);
    epd.refresh(0, 0, W, H);                      // partial
    epd.writeImageAgain(buf, 0, 0, W, H);
  }
  const unsigned long ms = millis() - t0;
  einkReleaseBus();
  memcpy(shown, buf, sizeof(shown));
  lastFrameEnd = millis();
  if (!full) {
    msSum += ms;
    if (++msFrames == 100) {
      Serial.printf("[EINK] rain: avg partial frame %lu ms\n", (unsigned long)(msSum / msFrames));
      msSum = msFrames = 0;
    }
  } else {
    Serial.printf("[EINK] rain: full refresh %lu ms\n", ms);
  }
}

// Get the controller ready to continue with partial refreshes.
// Returns true if the next frame must be a full refresh.
static bool panelResume() {
  switch (panel) {
    case PANEL_AWAKE:
      return false;
    case PANEL_ASLEEP:
      einkWakeKeep();                             // init without the forced full refresh
      einkDisplay.epd2.writeImageToPrevious(shown, 0, 0, W, H);   // what's on the glass
      einkReleaseBus();
      panel = PANEL_AWAKE;
      return false;
    default:
      einkWake();
      einkReleaseBus();
      panel = PANEL_AWAKE;
      return true;
  }
}

static void panelPause() {
  if (panel != PANEL_AWAKE) return;
  einkAcquireBus();
  einkDisplay.epd2.powerOff();                    // keeps the RAM, no deep sleep
  einkReleaseBus();
}

// ---------- public ----------
bool einkAnimRunning() { return running; }

void einkAnimInvalidate() {
  einkAnimStop();
  panel = PANEL_UNKNOWN;
  capped = false;
}

void einkAnimStop() {
  if (running) Serial.println(F("[EINK] rain paused"));
  running = false;
  if (panel == PANEL_AWAKE) {
    einkSleep();
    panel = PANEL_ASLEEP;
  }
}

void einkAnimBootFrame() {
  renderScene(frame);
  einkWake();
  sendFrame(true);
  einkSleep();
  panel = PANEL_ASLEEP;
}

void einkAnimTick() {
  if (!einkReady()) return;
  const unsigned long now = millis();
  const unsigned long activity = lastActivity();

  if (!running) {
    if (activity ? now - activity < IDLE_BEFORE_START_MS : now < IDLE_BEFORE_START_MS) return;
    if (capped && activity <= cappedAt) return;   // you've been away: stay still until you're back
    capped = false;
    running = true;
    runStart = now;
    const bool full = panelResume();
    renderScene(frame++);
    sendFrame(full);
    return;
  }

  if (activity > runStart) {                      // you're back: freeze on the current frame
    running = false;
    panelPause();
    return;
  }
  if (now - lastFrameEnd < FRAME_GAP_MS) return;

  if (now - runStart > MAX_RUN_MS) {              // away a long time: one clean refresh, then rest
    renderScene(frame);
    sendFrame(true);
    panelPause();
    running = false;
    capped = true;
    cappedAt = millis();
    Serial.println(F("[EINK] rain: resting after 30 min"));
    return;
  }

  renderScene(frame++);
  sendFrame(false);
}
