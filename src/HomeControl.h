#pragma once
#include <Arduino.h>

// Home Assistant control from the Fn layer (see AGENTS.md "Home Assistant").
// Fn tap = Home mode on/off, Fn+1..9 = pick device. In Home mode the knob
// drives the device: press = toggle, turn = adjust. HTTP runs in a background
// task; everything here is safe to call from loop() (and from the matrix scan
// that runs during E-Ink refreshes).

static const int HOME_MAX_SLOTS = 9;

void homeInit();            // load config from NVS; call in setup()
void homeLoop();            // timeouts; call every loop()

bool homeModeActive();
void homeToggleMode();      // Fn tap
void homeSelectSlot(int n); // Fn+N, 1-based; enters Home mode
void homeExit();
void homePress();           // knob press
void homeTurn(int steps);   // knob rotation (+ = clockwise)

bool homeConfigured();
int homeSlotCount();
int homeActiveSlot();       // 0-based
const char *homeSlotName(int i);
void homeStatusText(char *buf, size_t len);   // e.g. "ON 60%", "Wi-Fi...", "HA: bad token"
uint32_t homeStatusVersion();                 // bumps whenever the status text changes
uint32_t homePanelVersion();                  // bumps when the E-Ink panel needs a redraw

// Setup portal
String homeUrl();
String homeSlotsText();
bool homeHasToken();
void homeSaveConfig(const String &url, const String &token, const String &slots);  // empty token keeps the old one
