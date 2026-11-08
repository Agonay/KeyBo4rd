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
void oledDrawStatusLine();   // home screen; call every loop, redraws only on change
void oledForceRedraw();

// Short overlay on the home screen for knob actions.
void homeToast(const char *text);
void homeToastVolume(int steps);   // accumulates "VOL +N" while visible

#endif
