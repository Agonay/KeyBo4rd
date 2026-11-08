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

void menuInit();
KbdSettings &getSettings();
void saveSettings();

bool isMenuOpen();
void menuOpen();
void menuClose();

// Controls (see AGENTS.md "Menu system"):
//   list:  turn = move, press = select/edit, hold = close
//   edit:  turn = +/- , press = save,        hold = cancel (revert)
void menuEventShort();
void menuEventLong();
void menuRotate(int dir);

void menuDrawOled();

// Redraws on change and handles the inactivity auto-close; call from loop.
void menuRenderIfChanged();
void menuForceRender();

#endif
