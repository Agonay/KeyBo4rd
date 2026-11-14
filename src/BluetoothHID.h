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
// Notifications are fire-and-forget; sending without checking
// this lets an unbounded backlog build up (lag, stuck keys, disconnects).
bool bleCanSend();
uint16_t bleSendableCount();

// Link RSSI (dBm), sampled once a second from the loop. Take returns
// last/min/avg since the previous take; false if no sample (disconnected).
void bleSampleRssi(unsigned long now);
// 1 Hz history for the menu RSSI monitor; index 0 = oldest,
// RSSI_HISTORY_LEN-1 = newest. RSSI_NONE = not connected at that second.
static const int RSSI_HISTORY_LEN = 120;
static const int8_t RSSI_NONE = -128;
int8_t bleRssiHistory(int i);
bool bleTakeRssiStats(int8_t& last, int8_t& minimum, int8_t& average);

#endif
