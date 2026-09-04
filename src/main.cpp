#include <Arduino.h>
#include "BluetoothHID.h"
#include "KeyboardMatrix.h"
#include "DisplayManager.h"
#include "RotaryEncoder.h"
#include "BatteryManager.h"
#include "MenuManager.h"
#include <Wire.h>

void setup() {
    Serial.begin(115200);
    Wire.begin();
    Wire.setClock(400000);

    initBluetooth();
    delay(1000);
    initKeyboardMatrix();
    initDisplay();
    initRotaryEncoder();
    menuInit();
}

void loop() {
    // Poll BLE connection to detect reconnect edges (triggers releaseAll)
    isConnected();

    scanKeyboardMatrix();
    handleButtonPress();
    handleRotaryEncoder();

    // Update displays
    oledDrawStatusLine();
    menuRenderIfChanged();
    renderIdleScreen();
}
