#pragma once
#include <Arduino.h>

#ifndef FW_VERSION
#define FW_VERSION 0   // 0 = local dev build: automatic updates disabled
#endif

// Call in setup() after initDisplay() and BEFORE initBluetooth().
// Checks GitHub for a newer release and installs it (reboots on success).
void otaBootCheck();

// Call every loop(). Runs a background version check when the keyboard is idle.
void otaLoop();

// True while the update/countdown screen owns the OLED.
bool otaScreenActive();

// Menu "Check update": checks now (even when not idle) and installs right away
// if a newer release exists. Allowed on dev builds too, since it's explicit.
void otaCheckNow();
// Short status for the menu row: "", "Checking", "Latest", "No WiFi", "Failed", "vN found".
const char *otaStatusText();
// IP from the last Wi-Fi session ("off" if none yet).
const char *otaLastIp();

bool otaHasWifiCredentials();
String otaSavedSsid();
void otaSaveWifiCredentials(const String &ssid, const String &pass);
