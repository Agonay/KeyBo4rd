#include "KeyboardMatrix.h"
#include "Arduino.h"
#include <Wire.h>
#include <PCF8575.h>
#include "DisplayManager.h"
#include "BluetoothHID.h"

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

  // Row 5 — Bottom: Ctrl Win Alt Space AltGr <  ← ↓ → \ Backspace Delete
  {Key_Ctrl, Key_Cmd, Key_Left_Alt, Key_Space, Key_Right_Alt,
   Key_LessThan, Key_Acento,
   Key_ArrowLeft, Key_ArrowDown, Key_ArrowRight,
   Key_Backslash, Key_Backspace, Key_Delete},
};

// All physical states are sampled before building one complete HID report.
KeyState keyStates[numRows][numColsPCF];
const int rowPins[numRows] = {25, 33, 12, 26, 27, 14};
const uint8_t colPinsPCF[numColsPCF] = {P15, P13, P4, P1, P3, P7, P5, P6, P11, P0, P2, P14, P12};
static unsigned long lastScanTime = 0;
static unsigned long lastKeyActivityTime = 0;
static KeyReport lastReport = {};
static bool matrixReady = false;
static bool wasConnected = false;
static bool waitForRelease = false;

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
      }
      anyDown |= raw || state.isPressed;
    }
    gpioExpander.digitalWrite(colPinsPCF[col], HIGH);
  }
  const bool connected = isConnected();
  if (connected != wasConnected) {
    waitForRelease = true; // never replay a key held across a disconnection
    lastReport = {};
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
      if (!keyStates[row][col].isPressed) continue;
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
    bleKeyboard.sendReport(&report);
    lastReport = report;
  }
  // Repeat belongs to the host; held keys remain held for any duration.
}

unsigned long getLastKeyActivityTime() { return lastKeyActivityTime; }
