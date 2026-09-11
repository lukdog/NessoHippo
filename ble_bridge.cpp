// Nordic UART Service (NUS) bridge for the Nesso N1.
//
// One BLE characteristic each direction:
//   RX (write, no response) — desktop → device
//   TX (notify)             — device → desktop
//
// Lines on the wire are UTF-8 JSON objects terminated with '\n'.
// blePopLine() returns one complete line at a time so the protocol layer
// can deserialize independently of MTU fragmentation. bleSendLine()
// chops outgoing payloads into MTU-sized notifications.

#include "ble_bridge.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

static BLEServer*         s_server     = nullptr;
static BLECharacteristic* s_txChar     = nullptr;
static volatile bool      s_connected  = false;
static String             s_rxAssembly;     // accumulates bytes until '\n'
static String             s_pendingLines;   // complete '\n'-terminated lines
static portMUX_TYPE       s_rxMux      = portMUX_INITIALIZER_UNLOCKED;
static unsigned long      s_lastAdvKickMs = 0;

// How often bleTick() re-asserts advertising while disconnected. Starting
// advertising that is already running is a no-op, so this only costs a
// call every few seconds.
#define ADV_KICK_MS 5000UL

class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    s_connected = true;
  }
  void onDisconnect(BLEServer* srv) override {
    s_connected = false;
    // Resume advertising so the desktop can auto-reconnect.
    srv->getAdvertising()->start();
  }
};

class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = c->getValue();
    if (v.length() == 0) return;
    portENTER_CRITICAL(&s_rxMux);
    s_rxAssembly += v;
    int nl;
    while ((nl = s_rxAssembly.indexOf('\n')) >= 0) {
      // Move one full line (including '\n') to the pending queue.
      s_pendingLines += s_rxAssembly.substring(0, nl + 1);
      s_rxAssembly.remove(0, nl + 1);
    }
    portEXIT_CRITICAL(&s_rxMux);
  }
};

void bleBegin(const char* namePrefix) {
  // Build a per-device name. Suffix is the last 3 bytes of the BT MAC so
  // multiple Nesso buddies on the same bench stay distinguishable in the
  // desktop's picker (which filters by "Claude" prefix). We have to do a
  // throwaway init() first because getAddress() needs the controller up.
  BLEDevice::init("");
  const uint8_t* mac = BLEDevice::getAddress().getNative();
  char suffix[8];
  snprintf(suffix, sizeof(suffix), "-%02X%02X%02X", mac[3], mac[4], mac[5]);
  String fullName = String(namePrefix) + suffix;
  BLEDevice::deinit(false);
  BLEDevice::init(fullName.c_str());

  s_server = BLEDevice::createServer();
  s_server->setCallbacks(new ServerCb());

  BLEService* svc = s_server->createService(NUS_SERVICE_UUID);

  s_txChar = svc->createCharacteristic(
      NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  s_txChar->addDescriptor(new BLE2902());

  BLECharacteristic* rx = svc->createCharacteristic(
      NUS_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCb());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  // Advertise the name in the scan response so the picker sees it without
  // an extra connection round-trip.
  BLEAdvertisementData scanResp;
  scanResp.setName(fullName.c_str());
  adv->setScanResponseData(scanResp);
  adv->start();
}

bool bleIsConnected() {
  return s_connected;
}

// The transport's one unreliable moment: onDisconnect is a callback, and a
// peer whose radio simply goes away — a laptop suspending — can leave it
// undelivered. s_connected would then stay true forever, nothing would
// re-advertise, and the device would sit there invisible and convinced it
// was still paired. So trust the stack's own connection count over our
// flag, and re-assert advertising while there is nothing connected.
//
// Deliberately NOT done here: forcing a disconnect when heartbeats go
// quiet. A locked screen leaves the link up and idle, and tearing down a
// working link would be actively worse if the desktop does not reconnect
// by itself.
void bleTick() {
  if (!s_server) return;

  const bool reallyConnected = (s_server->getConnectedCount() > 0);
  if (s_connected != reallyConnected) {
    s_connected = reallyConnected;
  }
  if (reallyConnected) return;

  const unsigned long now = millis();
  if (now - s_lastAdvKickMs < ADV_KICK_MS) return;
  s_lastAdvKickMs = now;
  BLEDevice::startAdvertising();
}

bool blePopLine(String& out) {
  bool got = false;
  portENTER_CRITICAL(&s_rxMux);
  int nl = s_pendingLines.indexOf('\n');
  if (nl >= 0) {
    out = s_pendingLines.substring(0, nl); // strip trailing '\n'
    s_pendingLines.remove(0, nl + 1);
    got = true;
  }
  portEXIT_CRITICAL(&s_rxMux);
  return got;
}

void bleSendLine(const String& s) {
  if (!s_connected || !s_txChar) return;
  String payload = s + "\n";
  // Negotiated MTU - 3 bytes of ATT header. ESP32 default MTU is 23
  // (so 20-byte chunks); the desktop bumps this on connect, but using
  // 20 is always safe.
  const size_t CHUNK = 20;
  size_t n = payload.length();
  for (size_t off = 0; off < n; off += CHUNK) {
    size_t len = (n - off < CHUNK) ? (n - off) : CHUNK;
    s_txChar->setValue((uint8_t*)payload.c_str() + off, len);
    s_txChar->notify();
    // Tiny yield so back-to-back notifies don't get coalesced/dropped.
    delay(2);
  }
}

