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
// Row 0: F-keys + Escape (13 keys)
// Row 1: º 1-0 ' ¡ (13 keys)
// Row 2: Tab Q-P ´ + (13 keys)
// Row 3: Caps A-L Ñ ´ Enter (13 keys)
// Row 4: Shift Z-M , . - Shift ↑ (13 keys)
// Row 5: Ctrl Win Alt Space AltGr \ ← ↓ → \ Backspace Delete (13 keys)
const Key keyboardLayout[numRows][numColsPCF] = {
  // Row 0 — Function row
  {Key_Escape, Key_F1, Key_F2, Key_F3, Key_F4, Key_F5, Key_F6,
   Key_F7, Key_F8, Key_F9, Key_F10, Key_F11, Key_F12},

  // Row 1 — Number row: º 1 2 3 4 5 6 7 8 9 0 ' ¡
  {Key_Grado, Key_1, Key_2, Key_3, Key_4, Key_5, Key_6,
   Key_7, Key_8, Key_9, Key_0, Key_Apostrophe_ES, Key_ExclInv},

  // Row 2 — QWERTY row: Tab Q W E R T Y U I O P ´ +
  {Key_Tab, Key_Q, Key_W, Key_E, Key_R, Key_T, Key_Y,
   Key_U, Key_I, Key_O, Key_P, Key_Acento, Key_Mas},

  // Row 3 — Home row: Caps A S D F G H J K L Ñ ´ Enter
  {Key_CapsLock, Key_A, Key_S, Key_D, Key_F, Key_G, Key_H,
   Key_J, Key_K, Key_L, Key_Nie, Key_Acento, Key_Enter},

  // Row 4 — Bottom alpha: Shift Z X C V B N M , . - Shift ↑
  {Key_Shift, Key_Z, Key_X, Key_C, Key_V, Key_B, Key_N,
   Key_M, Key_Comma_ES, Key_Period_ES, Key_Guion, Key_Shift, Key_ArrowUp},

  // Row 5 — Bottom: Ctrl Win Alt Space AltGr \  ← ↓ → \ Backspace Delete
  {Key_Ctrl, Key_Cmd, Key_Left_Alt, Key_Space, Key_Right_Alt,
   Key_Backslash, Key_Acento,
   Key_ArrowLeft, Key_ArrowDown, Key_ArrowRight,
   Key_Backslash, Key_Backspace, Key_Delete},
};

// ---------- State ----------
KeyState keyStates[numRows][numColsPCF];

const int rowPins[numRows] = {25, 33, 12, 26, 27, 14};
const uint8_t colPinsPCF[numColsPCF] = {
  P15, P13, P4, P1, P3, P7, P5, P6, P11, P0, P2, P14, P12
};

const unsigned long debounceDelay = 100;
const unsigned long initialRepeatDelay = 500;
const unsigned long repeatRate = 80;

unsigned long lastScanTime = 0;
const unsigned long scanInterval = 10;

// Safety: force-release any key held > 2 s
const unsigned long maxHoldTimeMs = 2000;

void initKeyboardMatrix() {
  for (int i = 0; i < numColsPCF; i++) {
    gpioExpander.pinMode(colPinsPCF[i], OUTPUT);
    gpioExpander.digitalWrite(colPinsPCF[i], HIGH);
  }

  for (int i = 0; i < numRows; i++) {
    pinMode(rowPins[i], INPUT_PULLUP);
  }

  if (!gpioExpander.begin()) {
    Serial.println("Failed to initialize PCF8575!");
    while (1);
  }
  delay(1000);
  bleKeyboard.release(KEY_CAPS_LOCK);
  Serial.println("PCF8575 initialized successfully. Keyboard Matrix Initialized");
}

void scanKeyboardMatrix() {
  unsigned long currentTime = millis();

  if (currentTime - lastScanTime < scanInterval) return;
  lastScanTime = currentTime;

  bool shiftPressed = keyStates[4][0].isPressed || keyStates[4][11].isPressed;
  bool altPressed   = keyStates[5][2].isPressed || keyStates[5][4].isPressed;

  for (int col = 0; col < numColsPCF; col++) {
    gpioExpander.digitalWrite(colPinsPCF[col], LOW);
    delayMicroseconds(15);

    for (int row = 0; row < numRows; row++) {
      bool pressed = (digitalRead(rowPins[row]) == LOW);

      if (pressed) {
        if (!keyStates[row][col].isPressed) {
          // Fresh press
          keyStates[row][col].isPressed = true;
          keyStates[row][col].firstPressTime = currentTime;
          keyStates[row][col].lastRepeatTime = currentTime;

          Serial.printf("Key pressed row=%d col=%d shift=%d alt=%d\n",
            row, col, shiftPressed, altPressed);

          sendKeyPress(row, col, shiftPressed, altPressed);
        } else if (currentTime - keyStates[row][col].firstPressTime >= initialRepeatDelay) {
          // Auto-repeat
          if (currentTime - keyStates[row][col].lastRepeatTime >= repeatRate) {
            keyStates[row][col].lastRepeatTime = currentTime;
            sendKeyPress(row, col, shiftPressed, altPressed);
          }
        }
      } else {
        if (keyStates[row][col].isPressed) {
          keyStates[row][col].isPressed = false;
          releaseKey(row, col);
        }
      }
    }
    gpioExpander.digitalWrite(colPinsPCF[col], HIGH);
  }

  // Safety: force-release any key held too long
  for (int col = 0; col < numColsPCF; col++) {
    for (int row = 0; row < numRows; row++) {
      if (keyStates[row][col].isPressed &&
          (currentTime - keyStates[row][col].firstPressTime > maxHoldTimeMs)) {
        keyStates[row][col].isPressed = false;
        releaseKey(row, col);
        Serial.printf("Force release row=%d col=%d (held >%lu ms)\n",
          row, col, maxHoldTimeMs);
      }
    }
  }
}

void sendKeyPress(int row, int col, bool shiftPressed, bool altPressed) {
  if (!bleKeyboard.isConnected()) return;

  Key key = keyboardLayout[row][col];

  if (key.isModifier) {
    bleKeyboard.press(key.normalKey);
    if (key.normalKey == KEY_CAPS_LOCK) {
      capsLockStatus(isCapsFromHost());
    }
  } else {
    if (altPressed && key.altKey != 0) {
      bleKeyboard.press(key.altKey);
    } else if (shiftPressed && key.shiftKey != 0) {
      bleKeyboard.press(key.shiftKey);
    } else {
      bleKeyboard.press(key.normalKey);
    }
  }
}

void releaseKey(int row, int col) {
  if (!bleKeyboard.isConnected()) return;

  Key key = keyboardLayout[row][col];

  if (key.normalKey) bleKeyboard.release(key.normalKey);
  if (key.shiftKey)  bleKeyboard.release(key.shiftKey);
  if (key.altKey)    bleKeyboard.release(key.altKey);
}
