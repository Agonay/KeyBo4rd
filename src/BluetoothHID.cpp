#include "BluetoothHID.h"
#include <BLEDevice.h>
#include <esp_bt.h>
#include <esp_gatt_common_api.h>

static volatile uint8_t leds = 0;
static bool wasConnected = false;
static unsigned long connectedAt = 0;
static volatile uint16_t connId = 0;
class Keyboard : public BleKeyboard {
public:
  Keyboard() : BleKeyboard("K3YBo4rd", "AgonaySS", 100) {}
  void onWrite(BLECharacteristic* c) override {
    const std::string value = c->getValue();
    if (!value.empty()) leds = static_cast<uint8_t>(value[0]);
  }
  void onConnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    connId = param->connect.conn_id;
    // The core invokes the inherited one-argument callback as well.
    // 15-30 ms interval, no slave latency, 6 s supervision timeout.
    server->updateConnParams(param->connect.remote_bda, 12, 24, 0, 600);
    BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_CONN_HDL0);
  }
  void onDisconnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    // 0x08 = supervision timeout (radio link lost), 0x13 = host closed it,
    // 0x22/0x3E = link-layer failure. Logged from the BLE task; no BLE calls here.
    disconnectReason = param->disconnect.reason;
  }
public:
  static volatile int disconnectReason;
};
volatile int Keyboard::disconnectReason = -1;
static Keyboard keyboard;
BleKeyboard& bleKeyboard = keyboard;

void initBluetooth() {
  bleKeyboard.begin();
  BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_ADV);
  BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
}
bool isCapsFromHost() { return (leds & 2) != 0; }
bool isConnected() {
  const bool connected = bleKeyboard.isConnected();
  if (!connected) {
    if (wasConnected) Serial.printf("[BLE] disconnected (reason 0x%02X, heap %u); advertising restarted\n",
                                    Keyboard::disconnectReason & 0xFF, ESP.getFreeHeap());
    wasConnected = false;
    leds = 0;
    return false;
  }
  if (!wasConnected) {
    connectedAt = millis();
    wasConnected = true;
    Serial.println("[BLE] connected; waiting for host subscription");
  }
  // Give encryption/service subscription time before resetting the host report.
  static bool ready = false;
  if (millis() - connectedAt < 1000) { ready = false; return false; }
  if (!ready) {
    bleKeyboard.releaseAll();
    MediaKeyReport media = {};
    bleKeyboard.sendReport(&media);
    ready = true;
    Serial.println("[BLE] ready; reports cleared");
  }
  return true;
}

uint16_t bleSendableCount() {
  return bleKeyboard.isConnected() ? esp_ble_get_cur_sendable_packets_num(connId) : 0;
}
bool bleCanSend() { return bleSendableCount() > 0; }
