#include <Arduino.h>
#include "BluetoothHID.h"
#include "KeyboardMatrix.h"
#include "DisplayManager.h"
#include "RotaryEncoder.h"
#include "BatteryManager.h"
#include "MenuManager.h"
#include <Wire.h>
#include "FirmwareUpdate.h"
#include "OtaUpdater.h"

void setup() {
    Serial.begin(115200);
    if (startFirmwareUpdateIfRequested()) return;
    Wire.begin();
    Wire.setTimeOut(10);
    Wire.setClock(400000);

    menuInit();
    initKeyboardMatrix();
    initDisplay();
    initRotaryEncoder();
    otaBootCheck();      // before BLE: Wi-Fi + TLS get the radio and RAM to themselves
    initBluetooth();
}

void loop() {
    if (firmwareUpdateActive()) { handleFirmwareUpdate(); return; }
    // Poll BLE connection to detect reconnect edges (triggers releaseAll)
    isConnected();

    scanKeyboardMatrix();
    handleButtonPress();
    handleRotaryEncoder();
    otaLoop();

    // Update displays
    oledDrawStatusLine();
    menuRenderIfChanged();
    renderIdleScreen();
    delay(1);
}
