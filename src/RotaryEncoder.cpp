#include "RotaryEncoder.h"
#include <Arduino.h>
#include "DisplayManager.h"
#include "MenuManager.h"

// ---------- pins ----------
#define CLK_PIN 18
#define DT_PIN  19
#define SW_PIN  13

// ---------- quadrature decoder ----------
DRAM_ATTR const int8_t qdec_lut[16] = {
  0, -1, +1,  0,
 +1,  0,  0, -1,
 -1,  0,  0, +1,
  0, +1, -1,  0
};

const uint8_t STEPS_PER_DETENT = 4;
volatile long encoderTicks = 0;
volatile long encoderDetents = 0;
uint8_t       prevAB = 0;

// tiny noise filter (microseconds) — the LUT self-corrects; only skip extreme chatter
const unsigned long minEdgeUs = 100;
static volatile unsigned long lastEdgeUs = 0;

static void IRAM_ATTR encoderISR() {
  unsigned long nowUs = micros();
  if (nowUs - lastEdgeUs < minEdgeUs) return;
  lastEdgeUs = nowUs;

  uint8_t a = digitalRead(CLK_PIN);
  uint8_t b = digitalRead(DT_PIN);
  uint8_t ab = (a << 1) | b;
  if (ab == prevAB) return;

  uint8_t idx = (prevAB << 2) | ab;
  int8_t delta = qdec_lut[idx];
  if (delta != 0) {
    encoderTicks += delta;
    if (encoderTicks % STEPS_PER_DETENT == 0) {
      long newDetents = encoderTicks / STEPS_PER_DETENT;
      if (newDetents != encoderDetents) {
        encoderDetents = newDetents;
      }
    }
  }
  prevAB = ab;
}

// ---------- button FSM (non-blocking) ----------
static unsigned long pressTime = 0;
static bool btnDown = false;
static bool longFired = false;
static bool vlongFired = false;
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
    // fresh press
    btnDown     = true;
    pressTime   = now;
    longFired   = false;
    vlongFired  = false;
  } else if (!reading && btnDown) {
    // release
    btnDown = false;
    if (!longFired && !vlongFired) {
      pendingEvent = BTN_SHORT;
      Serial.println(F("[BTN] short"));
    }
  }

  if (btnDown) {
    unsigned long held = now - pressTime;
    if (!vlongFired && held >= vlongMs) {
      vlongFired = true;
      pendingEvent = BTN_VERYLONG;
      Serial.println(F("[BTN] very-long"));
    } else if (!longFired && !vlongFired && held >= longMs) {
      longFired = true;
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

static int pendingVolume = 0;
static bool pendingPlay = false;
static bool mediaDown = false;
static unsigned long mediaAt = 0;
static void serviceMedia() {
  if (!isConnected()) {
    pendingVolume = 0;
    pendingPlay = false;
    mediaDown = false;
    return;
  }
  if (millis() - mediaAt < 20) return;
  MediaKeyReport report = {};
  if (mediaDown) {
    mediaDown = false;
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

// ---------- rotation drain ----------
void handleRotaryEncoder() {
  serviceMedia();
  // button routing
  ButtonEvent ev = pollButtonEvent();
  if (ev != BTN_NONE) {
    // long press always opens/closes menu
    if (ev == BTN_LONG) {
      menuEventLong();
    } else if (isMenuOpen()) {
      switch (ev) {
        case BTN_SHORT:     menuEventShort();     break;
        case BTN_VERYLONG:  menuEventVeryLong();  break;
        default: break;
      }
    } else {
      switch (ev) {
        case BTN_SHORT:
          if (isConnected()) {
            pendingPlay = true;
            Serial.println(F("[BTN] media key sent"));
          } else {
            Serial.println(F("[BTN] BLE not connected"));
          }
          break;
        case BTN_VERYLONG:
          if (isConnected()) {
            bleKeyboard.releaseAll();
            Serial.println(F("[BTN] recovery: releaseAll"));
          }
          break;
        default:
          break;
      }
    }
  }

  // rotation drain

  long det = encoderDetents;
  static long lastDet = 0;
  if (det == lastDet) return;
  lastKnobActivity = millis();

  int dir = (det > lastDet) ? +1 : -1;
  int steps = min(4, (int)abs(det - lastDet));

  if (isMenuOpen()) {
    for (int i = 0; i < steps; ++i) menuRotate(dir);
  } else if (isConnected()) {
    KbdSettings &s = getSettings();
    uint8_t reps = s.volStepsPerDetent;
    if (s.volInvertDir) dir = -dir;

    pendingVolume = constrain(pendingVolume + dir * reps * steps, -32, 32);
    Serial.printf("ENC %s | det=%ld reps=%d\n",
      (dir > 0) ? "CW" : "CCW", det, reps);
  }

  lastDet += (det > lastDet ? steps : -steps);
}

void initRotaryEncoder() {
  pinMode(CLK_PIN, INPUT);
  pinMode(DT_PIN,  INPUT);
  pinMode(SW_PIN,  INPUT_PULLUP);

  prevAB = (digitalRead(CLK_PIN) << 1) | digitalRead(DT_PIN);

  attachInterrupt(digitalPinToInterrupt(CLK_PIN), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DT_PIN),  encoderISR, CHANGE);

  Serial.println(F("encoder initialized (CHANGE both pins, BLE in loop)"));
}
