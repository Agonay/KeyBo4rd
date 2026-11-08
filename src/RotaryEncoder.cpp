#include "RotaryEncoder.h"
#include <Arduino.h>
#include "DisplayManager.h"
#include "MenuManager.h"
#include "HomeControl.h"

// ---------- pins ----------
#define CLK_PIN 18
#define DT_PIN  19
#define SW_PIN  13

// This encoder is full-cycle: 4 transitions per click, always resting at A=B=1
// (measured with -DENC_DEBUG). A click is counted when the knob settles back in
// a rest state, and the count re-syncs there, so a dropped transition can't
// leave it out of step. Half-cycle encoders (rest at 00 and 11): -DENC_HALF_STEP.
#define ENC_REST_A 3
#ifdef ENC_HALF_STEP
#define ENC_REST_B 0
#else
#define ENC_REST_B 3
#endif

// ---------- quadrature decoder ----------
DRAM_ATTR const int8_t qdec_lut[16] = {
  0, -1, +1,  0,
 +1,  0,  0, -1,
 -1,  0,  0, +1,
  0, +1, -1,  0
};

volatile long encoderTicks = 0;
volatile long encoderDetents = 0;
static volatile long detentAnchor = 0;
uint8_t prevAB = 0;

// The lookup table rejects invalid jumps and contact bounce cancels itself
// (+1/-1), so no time filter is needed (one used to drop real edges).
static void IRAM_ATTR encoderISR() {
  const uint8_t ab = (digitalRead(CLK_PIN) << 1) | digitalRead(DT_PIN);
  if (ab == prevAB) return;
  const int8_t delta = qdec_lut[(prevAB << 2) | ab];
  prevAB = ab;
  if (!delta) return;
  encoderTicks += delta;
  if (ab == ENC_REST_A || ab == ENC_REST_B) {
    // >= 2 (not 4) so a click still counts if a transition was missed;
    // bouncing at the rest position nets 0 and counts nothing.
    const long d = encoderTicks - detentAnchor;
    if (d >= 2) encoderDetents++;
    else if (d <= -2) encoderDetents--;
    detentAnchor = encoderTicks;
  }
}

// ---------- button FSM (non-blocking) ----------
static const unsigned long DOUBLE_PRESS_MS = 300;
static unsigned long pressTime = 0;
static bool btnDown = false;
static bool longFired = false;
static bool vlongFired = false;
static unsigned long singleReleasedAt = 0;  // a short press waiting to see if a 2nd follows
static volatile ButtonEvent pendingEvent = BTN_NONE;
static unsigned long lastKnobActivity = 0;
unsigned long getLastKnobActivityTime() { return lastKnobActivity; }

void handleButtonPress() {
  unsigned long now = millis();
  static bool lastRaw = false;
  static bool stable = false;
  static unsigned long changedAt = 0;
  const bool raw = digitalRead(SW_PIN) == LOW;
  if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
  if (now - changedAt >= 25) stable = raw;
  if (stable) lastKnobActivity = now;
  const bool reading = stable;
  uint16_t longMs  = getSettings().longPressMs;
  uint16_t vlongMs = getSettings().veryLongPressMs;

  if (reading && !btnDown) {
    btnDown    = true;
    pressTime  = now;
    longFired  = false;
    vlongFired = false;
  } else if (!reading && btnDown) {
    btnDown = false;
    if (!longFired && !vlongFired) {
      if (isMenuOpen() || homeModeActive()) {
        pendingEvent = BTN_SHORT;              // menu/Home: no double-press, react instantly
      } else if (singleReleasedAt && now - singleReleasedAt <= DOUBLE_PRESS_MS) {
        singleReleasedAt = 0;
        pendingEvent = BTN_DOUBLE;
        Serial.println(F("[BTN] double"));
      } else {
        singleReleasedAt = now;                // decide after the double-press window
      }
    }
  }

  if (singleReleasedAt && !btnDown && now - singleReleasedAt > DOUBLE_PRESS_MS) {
    singleReleasedAt = 0;
    pendingEvent = BTN_SHORT;
    Serial.println(F("[BTN] short"));
  }

  if (btnDown) {
    unsigned long held = now - pressTime;
    if (!vlongFired && held >= vlongMs) {
      vlongFired = true;
      pendingEvent = BTN_VERYLONG;
      Serial.println(F("[BTN] very-long"));
    } else if (!longFired && !vlongFired && held >= longMs) {
      longFired = true;
      singleReleasedAt = 0;
      pendingEvent = BTN_LONG;
      Serial.println(F("[BTN] long"));
    }
  }
}

ButtonEvent pollButtonEvent() {
  ButtonEvent ev = pendingEvent;
  pendingEvent = BTN_NONE;
  return ev;
}

// ---------- media keys (sent from loop, paced) ----------
static int pendingVolume = 0;
static bool pendingPlay = false;
static bool pendingMute = false;
static bool mediaDown = false;
static unsigned long mediaAt = 0;
static void serviceMedia() {
  if (!isConnected()) {
    pendingVolume = 0;
    pendingPlay = false;
    pendingMute = false;
    mediaDown = false;
    return;
  }
  if (millis() - mediaAt < 20) return;
  if (!bleCanSend()) return;   // link busy: keep pending steps for later
  MediaKeyReport report = {};
  if (mediaDown) {
    mediaDown = false;
  } else if (pendingMute) {
    report[0] = KEY_MEDIA_MUTE[0];
    pendingMute = false;
    mediaDown = true;
  } else if (pendingPlay) {
    report[0] = KEY_MEDIA_PLAY_PAUSE[0];
    pendingPlay = false;
    mediaDown = true;
  } else if (pendingVolume) {
    report[0] = pendingVolume > 0 ? KEY_MEDIA_VOLUME_UP[0] : KEY_MEDIA_VOLUME_DOWN[0];
    pendingVolume += pendingVolume > 0 ? -1 : 1;
    mediaDown = true;
  } else return;
  bleKeyboard.sendReport(&report);
  mediaAt = millis();
}

// Fast spinning multiplies volume steps.
static int accelFactor(unsigned long sinceLastMs) {
  if (sinceLastMs < 25) return 3;
  if (sinceLastMs < 50) return 2;
  return 1;
}

// ---------- event routing ----------
void handleRotaryEncoder() {
  serviceMedia();

  ButtonEvent ev = pollButtonEvent();
  if (ev == BTN_VERYLONG) {
    // Recovery for stuck keys; never changes the menu (it used to close the
    // menu the long press had just opened).
    if (isConnected()) {
      bleKeyboard.releaseAll();
      Serial.println(F("[BTN] recovery: releaseAll"));
      if (!isMenuOpen()) homeToast("KEYS FREED");
    }
  } else if (isMenuOpen()) {
    if (ev == BTN_SHORT) menuEventShort();
    else if (ev == BTN_LONG) menuEventLong();
  } else if (homeModeActive()) {
    if (ev == BTN_SHORT) homePress();
    else if (ev == BTN_LONG) homeExit();
  } else {
    switch (ev) {
      case BTN_LONG:
        menuOpen();
        break;
      case BTN_SHORT:
        if (isConnected()) { pendingPlay = true; homeToast("PLAY/PAUSE"); }
        break;
      case BTN_DOUBLE:
        if (isConnected()) { pendingMute = true; homeToast("MUTE"); }
        break;
      default:
        break;
    }
  }

#ifdef ENC_DEBUG
  { static long lt = 0; long t = encoderTicks; if (t != lt) { lt = t; Serial.printf("[ENCDBG] ticks=%ld det=%ld ab=%d\n", t, (long)encoderDetents, prevAB); } }
#endif

  long det = encoderDetents;
  static long lastDet = 0;
  static unsigned long lastDetAt = 0;
  if (det == lastDet) return;
  const unsigned long now = millis();
  lastKnobActivity = now;

  int dir = (det > lastDet) ? +1 : -1;
  int steps = min(4, (int)abs(det - lastDet));

  if (isMenuOpen()) {
    for (int i = 0; i < steps; ++i) menuRotate(dir);
  } else if (homeModeActive()) {
    homeTurn((getSettings().volInvertDir ? -dir : dir) * steps);
  } else if (isConnected()) {
    KbdSettings &s = getSettings();
    const int accel = accelFactor(now - lastDetAt);
    if (s.volInvertDir) dir = -dir;
    const int delta = dir * s.volStepsPerDetent * steps * accel;
    pendingVolume = constrain(pendingVolume + delta, -32, 32);
    homeToastVolume(delta);
    Serial.printf("ENC %s | det=%ld x%d\n", (dir > 0) ? "CW" : "CCW", det, accel);
  }

  lastDetAt = now;
  lastDet += (det > lastDet ? steps : -steps);
}

static bool encoderReady = false;
static bool encoderAttached = false;

void encoderSuspend() {
  if (!encoderAttached) return;
  detachInterrupt(digitalPinToInterrupt(CLK_PIN));
  detachInterrupt(digitalPinToInterrupt(DT_PIN));
  encoderAttached = false;
}

void encoderResume() {
  if (!encoderReady || encoderAttached) return;
  pinMode(CLK_PIN, INPUT);
  pinMode(DT_PIN,  INPUT);
  prevAB = (digitalRead(CLK_PIN) << 1) | digitalRead(DT_PIN);
  detentAnchor = encoderTicks;   // a half-turn made while paused must not count
  attachInterrupt(digitalPinToInterrupt(CLK_PIN), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DT_PIN),  encoderISR, CHANGE);
  encoderAttached = true;
}

void initRotaryEncoder() {
  pinMode(SW_PIN, INPUT_PULLUP);
  encoderReady = true;
  encoderResume();

  Serial.println(F("encoder initialized (counts on rest position)"));
}
