#ifndef BLUETOOTH_HID_H
#define BLUETOOTH_HID_H

#include <BleKeyboard.h>
#include <Arduino.h>

extern BleKeyboard& bleKeyboard;

void initBluetooth();
bool isCapsFromHost();

// Returns true if we just reconnected (for releaseAll)
bool isConnected();

#endif
