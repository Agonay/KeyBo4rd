#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <Fonts/FreeMonoBold9pt7b.h>

#include "MenuManager.h"

extern Adafruit_SSD1306 oled;
extern GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> einkDisplay;

void initDisplay();                                  // OLED boot animation + E-Ink boot image
void bootSplashStatus(const char *status, int pct);  // boot splash: status line + progress bar
void applyOledBrightness(uint8_t percent);
void renderIdleScreen();

// E-Ink bus/power (GPIO18 is shared with the knob CLK, see HardwareConfig.h).
// Wake = SPI + init (first refresh is full); Sleep = hibernate + give the pin back.
// Acquire/Release only move SPI, so the panel keeps power and RAM between
// partial frames while the knob works in the gaps.
bool einkReady();
void einkWake();
void einkWakeKeep();   // wake without the forced first full refresh
void einkSleep();
void einkAcquireBus();
void einkReleaseBus();
void oledDrawStatusLine();   // home screen; call every loop, redraws only on change
void oledForceRedraw();

// Short overlay on the home screen for knob actions.
void homeToast(const char *text);
void homeToastVolume(int steps);   // accumulates "VOL +N" while visible

#endif
