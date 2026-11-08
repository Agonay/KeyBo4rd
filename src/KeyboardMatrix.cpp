#include "KeyboardMatrix.h"
#include "Arduino.h"
#include <Wire.h>
#include <PCF8575.h>
#include "DisplayManager.h"
#include "BluetoothHID.h"
#include "HomeControl.h"

// ---------- GPIO extender ----------
PCF8575 gpioExpander(0x20);

// ---------- Matrix size ----------
const int numRows = 6;
const int numColsPCF = 13;

// ---------- Layout table (Spanish ES on ANSI physical) ----------
const Key keyboardLayout[numRows][numColsPCF] = {
  // Row 0 — Function row
  {Key_Escape, Key_F1, Key_F2, Key_F3, Key_F4, Key_F5, Key_F6,
   Key_F7, Key_F8, Key_F9, Key_F10, Key_F11, Key_F12},

  // Row 1 — Number row: º 1 2 3 4 5 6 7 8 9 0 ' ¡
  {Key_Grado, Key_1, Key_2, Key_3, Key_4, Key_5, Key_6,
   Key_7, Key_8, Key_9, Key_0, Key_Apostrophe_ES, Key_ExclInv},

  // Row 2 — QWERTY row: Tab Q W E R T Y U I O P [ ]
  {Key_Tab, Key_Q, Key_W, Key_E, Key_R, Key_T, Key_Y,
   Key_U, Key_I, Key_O, Key_P, Key_LeftBracket, Key_RightBracket},

  // Row 3 — Home row: Caps A S D F G H J K L Ñ ´ Enter
  {Key_CapsLock, Key_A, Key_S, Key_D, Key_F, Key_G, Key_H,
   Key_J, Key_K, Key_L, Key_Nie, Key_Acento, Key_Enter},

  // Row 4 — Bottom alpha: Shift Z X C V B N M , . - Shift ↑
  {Key_Shift, Key_Z, Key_X, Key_C, Key_V, Key_B, Key_N,
   Key_M, Key_Comma_ES, Key_Period_ES, Key_Guion, Key_Shift, Key_ArrowUp},

  // Row 5 — Bottom: Ctrl Win Alt Space AltGr < Fn ← ↓ → \ Backspace Delete
  {Key_Ctrl, Key_Cmd, Key_Left_Alt, Key_Space, Key_Right_Alt,
   Key_LessThan, Key_Fn,
   Key_ArrowLeft, Key_ArrowDown, Key_ArrowRight,
   Key_Backslash, Key_Backspace, Key_Delete},
};

// Fn key position (left of the left arrow).
static const int FN_ROW = 5, FN_COL = 6;

// All physical states are sampled before building one complete HID report.
KeyState keyStates[numRows][numColsPCF];
const int rowPins[numRows] = {25, 33, 12, 26, 27, 14};
const uint8_t colPinsPCF[numColsPCF] = {P15, P13, P4, P1, P3, P7, P5, P6, P11, P0, P2, P14, P12};
static unsigned long lastScanTime = 0;
static unsigned long lastKeyActivityTime = 0;
static uint32_t keyPressCount = 0;
static KeyReport lastReport = {};
static bool matrixReady = false;
static bool wasConnected = false;
static bool waitForRelease = false;

// ---------- paced report queue ----------
// BLE notifications don't block: anything sent faster than the link can
// carry piles up inside the stack (lag, releases arriving seconds late so the
// host auto-repeats, then a disconnect). Reports wait here instead and leave
// only when the stack has a free TX buffer. Every report is a full snapshot,
// so when the queue is full the newest entry is overwritten: it never grows.
static const unsigned QUEUE_LEN = 16;
static const unsigned long MIN_SEND_GAP_MS = 8;
static KeyReport reportQueue[QUEUE_LEN];
static unsigned qHead = 0, qCount = 0;
static unsigned long lastSendMs = 0;
static uint32_t qCollapsed = 0;
static unsigned long queuedAt[QUEUE_LEN];

// Diagnostics, reset every log line: where does the time go when keys lag?
static unsigned long maxScanGap = 0;   // loop blocked: keys aren't even read
static unsigned long maxWait = 0;      // report waited for the BLE stack: link slow
static unsigned maxQ = 0;

static void queueClear() { qHead = qCount = 0; }

static void queuePush(const KeyReport &r, unsigned long now) {
  if (qCount == QUEUE_LEN) {
    reportQueue[(qHead + qCount - 1) % QUEUE_LEN] = r;
    ++qCollapsed;
    return;
  }
  const unsigned slot = (qHead + qCount) % QUEUE_LEN;
  reportQueue[slot] = r;
  queuedAt[slot] = now;
  ++qCount;
  if (qCount > maxQ) maxQ = qCount;
}

static void drainReports(unsigned long now) {
  if (!qCount || now - lastSendMs < MIN_SEND_GAP_MS || !bleCanSend()) return;
  const unsigned long waited = now - queuedAt[qHead];
  if (waited > maxWait) maxWait = waited;
  if (waited >= 150) Serial.printf("[DIAG] report waited %lu ms for the BLE stack (q=%u)\n", waited, qCount);
  bleKeyboard.sendReport(&reportQueue[qHead]);
  qHead = (qHead + 1) % QUEUE_LEN;
  --qCount;
  lastSendMs = now;
}

static void logLinkStats(unsigned long now) {
  static unsigned long lastLog = 0;
  if (now - lastLog < 10000) return;
  lastLog = now;
  Serial.printf("[BLE] heap=%u minHeap=%u sendable=%u q=%u maxQ=%u maxWait=%lums "
                "maxScanGap=%lums collapsed=%lu keys=%lu\n",
                ESP.getFreeHeap(), ESP.getMinFreeHeap(), bleSendableCount(), qCount, maxQ,
                maxWait, maxScanGap, (unsigned long)qCollapsed, (unsigned long)keyPressCount);
  maxQ = 0; maxWait = 0; maxScanGap = 0;
}

// ---------- Fn layer ----------
// Fn tap (no other key while held) = Home mode on/off; Fn+1..9 = Home device.
// Any key pressed while Fn is held is "consumed": it stays out of HID reports
// until released, even if Fn is let go first.
static bool fnUsed = false;

static void onKeyEdge(int row, int col, bool down) {
  KeyState &st = keyStates[row][col];
  if (row == FN_ROW && col == FN_COL) {
    if (down) fnUsed = false;
    else if (!fnUsed) homeToggleMode();
    return;
  }
  if (!down) { st.fnConsumed = false; return; }
  if (!keyStates[FN_ROW][FN_COL].isPressed) return;
  st.fnConsumed = true;
  fnUsed = true;
  const uint8_t u = keyboardLayout[row][col].usage;
  if (u >= Key_1.usage && u <= Key_9.usage) homeSelectSlot(u - Key_1.usage + 1);
}

void initKeyboardMatrix() {
  for (int col = 0; col < numColsPCF; ++col) {
    gpioExpander.pinMode(colPinsPCF[col], OUTPUT);
    gpioExpander.digitalWrite(colPinsPCF[col], HIGH);
  }
  for (int pin : rowPins) pinMode(pin, INPUT_PULLUP);
  matrixReady = gpioExpander.begin();
  Serial.println(matrixReady ? "[MATRIX] ready" : "[MATRIX] PCF8575 unavailable; check wiring");
}

void scanKeyboardMatrix() {
  const unsigned long now = millis();
  if (!matrixReady || now - lastScanTime < 4) return;
  if (lastScanTime) {
    const unsigned long gap = now - lastScanTime;
    if (gap > maxScanGap) maxScanGap = gap;
    if (gap >= 150) Serial.printf("[DIAG] keyboard scan blocked for %lu ms\n", gap);
  }
  lastScanTime = now;
  bool anyDown = false;
  for (int col = 0; col < numColsPCF; ++col) {
    gpioExpander.digitalWrite(colPinsPCF[col], LOW);
    delayMicroseconds(15);
    for (int row = 0; row < numRows; ++row) {
      const bool raw = digitalRead(rowPins[row]) == LOW;
      KeyState &state = keyStates[row][col];
      if (raw != state.lastRawState) {
        state.lastRawState = raw;
        state.stateChangeTime = now;
      }
      if (now - state.stateChangeTime >= 10 && raw != state.isPressed) {
        state.isPressed = raw;
        lastKeyActivityTime = now;
        onKeyEdge(row, col, raw);
        if (raw && keyboardLayout[row][col].usage) ++keyPressCount; // stats for the home screen
      }
      anyDown |= raw || state.isPressed;
    }
    gpioExpander.digitalWrite(colPinsPCF[col], HIGH);
  }
  const bool connected = isConnected();
  if (connected != wasConnected) {
    waitForRelease = true; // never replay a key held across a disconnection
    lastReport = {};
    queueClear();
    wasConnected = connected;
  }
  if (!connected) return;
  if (waitForRelease) {
    if (anyDown) return;
    waitForRelease = false;
  }
  KeyReport report = {};
  unsigned count = 0;
  for (int row = 0; row < numRows; ++row) {
    for (int col = 0; col < numColsPCF; ++col) {
      if (!keyStates[row][col].isPressed || keyStates[row][col].fnConsumed) continue;
      const Key &key = keyboardLayout[row][col];
      report.modifiers |= key.modifiers;
      if (!key.usage) continue;
      bool duplicate = false;
      for (unsigned i = 0; i < count && i < 6; ++i) duplicate |= report.keys[i] == key.usage;
      if (!duplicate) {
        if (count < 6) report.keys[count] = key.usage;
        ++count;
      }
    }
  }
  if (count > 6) memset(report.keys, 1, sizeof(report.keys)); // HID ErrorRollOver
  if (memcmp(&report, &lastReport, sizeof(report)) != 0) {
    queuePush(report, now);
    lastReport = report;
  }
  drainReports(now);
  logLinkStats(now);
  // Repeat belongs to the host; held keys remain held for any duration.
}

unsigned long getLastKeyActivityTime() { return lastKeyActivityTime; }
uint32_t getKeyPressCount() { return keyPressCount; }
