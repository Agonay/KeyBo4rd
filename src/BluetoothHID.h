#ifndef BLUETOOTH_HID_H
#define BLUETOOTH_HID_H

#include <BleKeyboard.h>
#include <Arduino.h>

extern BleKeyboard& bleKeyboard;

void initBluetooth();
bool isCapsFromHost();

// Ready after connection settling and clearing keyboard/media reports.
bool isConnected();

// Flow control: true when the BLE stack has a free TX buffer for this link.
// Notifications are fire-and-forget in Bluedroid; sending without checking
// this lets an unbounded backlog build up (lag, stuck keys, disconnects).
bool bleCanSend();
uint16_t bleSendableCount();

#endif
