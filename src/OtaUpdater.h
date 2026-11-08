#pragma once
#include <Arduino.h>

#ifndef FW_VERSION
#define FW_VERSION 0   // 0 = local dev build: automatic updates disabled
#endif

// Versions are MAJOR.MINOR, stored as one integer: MAJOR * 1000 + MINOR (6.1 = 6001).
// Releases before this scheme used plain build numbers (1..999).
// Writes "6.1" (or "6" for an old build number) into buf.
inline const char *fwVersionLabel(long code, char *buf, size_t len) {
  if (code >= 1000) snprintf(buf, len, "%ld.%ld", code / 1000, code % 1000);
  else snprintf(buf, len, "%ld", code);
  return buf;
}
inline String fwVersionString(long code) { char b[16]; return String(fwVersionLabel(code, b, sizeof(b))); }

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
