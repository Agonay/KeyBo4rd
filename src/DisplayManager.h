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

void initDisplay();
void capsLockStatus(bool on);
void batteryStatus(int charge);
void drawChargeTopLeft(uint8_t percent, uint8_t segments);
void demoFourStatesRow(int y);
void drawBattery24x8(int x, int y, uint8_t level);
void menuDemo();
void einkAnimateCampfireRain(uint32_t duration_ms);
void applyOledBrightness(uint8_t percent);
void drawEinkStatusDashboard();
void renderIdleScreen();
void drawEinkClock();
void oledDrawStatusLine();

#endif
