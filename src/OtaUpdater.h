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

bool otaHasWifiCredentials();
String otaSavedSsid();
void otaSaveWifiCredentials(const String &ssid, const String &pass);
