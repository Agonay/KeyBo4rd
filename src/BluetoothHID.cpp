#include <Arduino.h>
#include "BluetoothHID.h"
#include "DisplayManager.h"

static volatile uint8_t s_leds = 0;
static bool s_wasConnected = false;

#if defined(USE_NIMBLE)
  #include <NimBLECharacteristic.h>
  class MyBleKeyboard : public BleKeyboard {
  public:
    MyBleKeyboard() : BleKeyboard() {}
    MyBleKeyboard(const char* name, const char* mfg, uint8_t batt=100)
      : BleKeyboard(name, mfg, batt) {}

    void onWrite(NimBLECharacteristic* c) override {
      std::string v = c->getValue();
      if (v.empty()) return;
      uint8_t leds = (uint8_t)v[0];
      if (leds != s_leds) {
        s_leds = leds;
        Serial.printf("[BLE] CapsLock: %s\n", (leds & 0x02) ? "ON" : "OFF");
      }
    }
  };
#else
  #include <BLECharacteristic.h>
  class MyBleKeyboard : public BleKeyboard {
  public:
    MyBleKeyboard() : BleKeyboard() {}
    MyBleKeyboard(const char* name, const char* mfg, uint8_t batt=100)
      : BleKeyboard(name, mfg, batt) {}

    void onWrite(BLECharacteristic* c) override {
      const uint8_t* d = (const uint8_t*)c->getData();
      if (!d) return;
      uint8_t leds = *d;
      if (leds != s_leds) {
        s_leds = leds;
        Serial.printf("[BLE] CapsLock: %s\n", (leds & 0x02) ? "ON" : "OFF");
      }
    }
  };
#endif

static MyBleKeyboard keyboard("K3YBo4rd", "AgonaySS", 100);
BleKeyboard& bleKeyboard = keyboard;

void initBluetooth() {
  bleKeyboard.begin();
  s_wasConnected = false;
}

bool isCapsFromHost() {
  return (s_leds & 0x02) != 0;
}

// Track connection edge and clear latched keys on reconnect
bool isConnected() {
  bool connected = bleKeyboard.isConnected();
  if (connected && !s_wasConnected) {
    // Just reconnected — release all latched keys to prevent spam
    s_wasConnected = true;
    bleKeyboard.releaseAll();
    Serial.println(F("[BLE] reconnected → releaseAll"));
    return false; // caller should skip sending this cycle
  }
  if (!connected) {
    s_wasConnected = false;
  }
  return connected;
}
