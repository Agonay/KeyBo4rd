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
#include "HomeControl.h"

void setup() {
    Serial.begin(115200);
    if (startFirmwareUpdateIfRequested()) return;
    Wire.begin();
    Wire.setTimeOut(10);
    Wire.setClock(400000);

    menuInit();
    homeInit();
    initKeyboardMatrix();
    initDisplay();
    initRotaryEncoder();
    bootSplashStatus("Checking updates", 45);
    otaBootCheck();      // before BLE: Wi-Fi + TLS get the radio and RAM to themselves
    bootSplashStatus("Starting Bluetooth", 85);
    initBluetooth();
    bootSplashStatus("Ready", 100);
    delay(250);
    oledForceRedraw();   // hand the OLED to the home screen
}

void loop() {
    if (firmwareUpdateActive()) { handleFirmwareUpdate(); return; }
    // Poll BLE connection to detect reconnect edges (triggers releaseAll)
    isConnected();

    scanKeyboardMatrix();
    handleButtonPress();
    handleRotaryEncoder();
    otaLoop();
    homeLoop();

    // Update displays
    if (!otaScreenActive()) oledDrawStatusLine();
    menuRenderIfChanged();
    renderIdleScreen();
    delay(1);
}
