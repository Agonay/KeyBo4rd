#ifndef BLUETOOTH_HID_H
#define BLUETOOTH_HID_H

#include <BleKeyboard.h>
#include <Arduino.h>

extern BleKeyboard& bleKeyboard;

void initBluetooth();
bool isCapsFromHost();

// Ready after connection settling and clearing keyboard/media reports.
bool isConnected();

#endif
