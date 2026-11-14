#include "BluetoothHID.h"
#include <NimBLEDevice.h>

// NimBLE host: controller ACL buffers the host may still fill (ble_hs_hci.c).
// When it hits 0, further notifications queue inside the host's mbuf pool,
// the same backlog Bluedroid built up. Single link, so it is per-connection.
extern "C" uint16_t ble_hs_hci_avail_pkts;

static volatile uint8_t leds = 0;
static bool wasConnected = false;
static unsigned long connectedAt = 0;
static volatile int disconnectReason = -1;
static volatile uint16_t connHandle = 0xFFFF;  // BLE_HS_CONN_HANDLE_NONE

// RSSI of the link as the keyboard hears the laptop, sampled once a second.
static int8_t rssiLast = 0, rssiMin = 0;
static int32_t rssiSum = 0;
static uint16_t rssiCount = 0;

class Keyboard : public BleKeyboard {
public:
  Keyboard() : BleKeyboard("K3YBo4rd", "AgonaySS", 100) {}
  void onWrite(NimBLECharacteristic* c) override {
    const NimBLEAttValue value = c->getValue();
    if (value.length()) leds = value.data()[0];
  }
  // NimBLE calls both overloads; the one-argument one (base) sets `connected`.
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    connHandle = desc->conn_handle;
    // 15-30 ms interval, no slave latency, 6 s supervision timeout.
    server->updateConnParams(desc->conn_handle, 12, 24, 0, 600);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_CONN_HDL0);
  }
};
static Keyboard keyboard;
BleKeyboard& bleKeyboard = keyboard;

// Only for the disconnect reason, which NimBLE 1.4 server callbacks don't pass.
// Runs in the host task: no BLE calls, no Serial.
static int gapListener(ble_gap_event* event, void*) {
  if (event->type == BLE_GAP_EVENT_DISCONNECT) disconnectReason = event->disconnect.reason;
  return 0;
}

void initBluetooth() {
  // The library spin-waits this long after every report in NimBLE mode; pacing is
  // done by the report queue in KeyboardMatrix.cpp instead.
  bleKeyboard.setDelay(0);
  bleKeyboard.begin();  // advertising restarts on disconnect (NimBLE default)
  NimBLEDevice::setCustomGapHandler(gapListener);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_ADV);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
}
bool isCapsFromHost() { return (leds & 2) != 0; }
bool isConnected() {
  const bool connected = bleKeyboard.isConnected();
  if (!connected) {
    // 0x08 = supervision timeout (radio link lost), 0x13 = host closed it,
    // 0x22/0x3E = link-layer failure. NimBLE adds 0x200 to HCI reasons.
    if (wasConnected) Serial.printf("[BLE] disconnected (reason 0x%02X, heap %u); advertising restarted\n",
                                    disconnectReason & 0xFF, ESP.getFreeHeap());
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
  return bleKeyboard.isConnected() ? ble_hs_hci_avail_pkts : 0;
}
bool bleCanSend() { return bleSendableCount() > 0; }

static int8_t rssiHist[RSSI_HISTORY_LEN];
static uint16_t rssiHistHead = 0;   // next write slot
static bool rssiHistInit = false;

static void pushHistory(int8_t v) {
  if (!rssiHistInit) {
    for (auto &h : rssiHist) h = RSSI_NONE;
    rssiHistInit = true;
  }
  rssiHist[rssiHistHead] = v;
  rssiHistHead = (rssiHistHead + 1) % RSSI_HISTORY_LEN;
}

void bleSampleRssi(unsigned long now) {
  static unsigned long lastSample = 0;
  if (now - lastSample < 1000) return;
  lastSample = now;
  int8_t rssi;
  // One HCI command (~1 ms); 127 means the controller has no reading.
  if (!bleKeyboard.isConnected() || connHandle == 0xFFFF ||
      ble_gap_conn_rssi(connHandle, &rssi) != 0 || rssi == 127) {
    pushHistory(RSSI_NONE);  // gap in the graph = no link
    return;
  }
  pushHistory(rssi);
  rssiLast = rssi;
  if (rssiCount == 0 || rssi < rssiMin) rssiMin = rssi;
  rssiSum += rssi;
  ++rssiCount;
}

int8_t bleRssiHistory(int i) {
  if (!rssiHistInit || i < 0 || i >= RSSI_HISTORY_LEN) return RSSI_NONE;
  return rssiHist[(rssiHistHead + i) % RSSI_HISTORY_LEN];
}

bool bleTakeRssiStats(int8_t& last, int8_t& minimum, int8_t& average) {
  if (rssiCount == 0) return false;
  last = rssiLast;
  minimum = rssiMin;
  average = (int8_t)(rssiSum / rssiCount);
  rssiSum = 0;
  rssiCount = 0;
  return true;
}
