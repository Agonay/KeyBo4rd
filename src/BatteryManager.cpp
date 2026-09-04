#include "BatteryManager.h"
#include <Arduino.h>

static int batteryPercentage = 100;
static bool charging = false;
static unsigned long lastDrainTime = 0;
static const unsigned long drainIntervalMs = 60000; // drain 1% per minute (simulated)

int getBatteryPercentage() {
  unsigned long now = millis();
  if (now - lastDrainTime >= drainIntervalMs) {
    lastDrainTime = now;
    if (!charging) {
      batteryPercentage -= 1;
      if (batteryPercentage <= 0) batteryPercentage = 100;
    }
  }
  return batteryPercentage;
}

bool isCharging() {
  return charging;
}
