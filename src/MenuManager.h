#ifndef MENU_MANAGER_H
#define MENU_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>

enum MenuIdleMode {
  IdleTotoro,
  IdleStatus,
  IdleAnimation,
  IdleClock,
  IdleCount
};

struct KbdSettings {
  uint8_t volStepsPerDetent;    // 1–4, default 2
  bool    volInvertDir;         // default false
  uint8_t oledBrightness;       // 0–100 %, default 100
  MenuIdleMode einkIdleMode;    // default IdleTotoro
  bool    showBattery;          // default true
  uint8_t lowBatteryPct;        // default 15
  uint16_t longPressMs;         // default 800
  uint16_t veryLongPressMs;     // default 2500
};

static const int MENU_ROW_COUNT = 6;

void menuInit();
KbdSettings &getSettings();
void saveSettings();

bool isMenuOpen();
void menuOpen();
void menuClose();

// Call these when button events happen
void menuEventShort();   // increment highlighted value
void menuEventLong();    // back / close
void menuEventVeryLong();

// Rotation: scroll highlight when menu is open
void menuRotate(int dir);

// Returns the current highlight index (0..MENU_ROW_COUNT-1)
int menuHighlightIndex();

// Draw the current menu screen (call from DisplayManager)
void menuDrawEink();
void menuDrawOled();

// Only redraw if state changed; call from loop
void menuRenderIfChanged();

// Force a full menu redraw (e.g. after value change)
void menuForceRender();

#endif
