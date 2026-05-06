#include <NimBLEDevice.h>

// ── Peripheral side (toward Apple Watch) ──────────────────────────────────
NimBLECharacteristic* pHRChar;
bool watchConnected = false;

// ── Central side (toward real HR sensors) ────────────────────────────────
NimBLEClient* pClient = nullptr;
uint8_t lastBPM = 70; // fallback during sensor switch

// Addresses of your two sensors (find via BLE scan)
const char* SENSOR_A = "AA:BB:CC:DD:EE:FF";
const char* SENSOR_B = "11:22:33:44:55:66";
const char* activeSensor = SENSOR_A;

// ── Peripheral callbacks ──────────────────────────────────────────────────
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s)    { watchConnected = true; }
  void onDisconnect(NimBLEServer* s) {
    watchConnected = false;
    NimBLEDevice::startAdvertising();
  }
};

// ── HR notification from real sensor → forward to watch ──────────────────
void hrNotifyCallback(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
  lastBPM = data[1]; // assumes uint8 BPM format (flags byte = 0x00)
  if (watchConnected) {
    pHRChar->setValue(data, len); // forward raw packet as-is
    pHRChar->notify();
  }
}

// ── Connect to a sensor by address ───────────────────────────────────────
bool connectToSensor(const char* address) {
  if (pClient && pClient->isConnected()) pClient->disconnect();

  pClient = NimBLEDevice::createClient();
  NimBLEAddress addr(std::string(address), BLE_ADDR_PUBLIC);

  if (!pClient->connect(addr)) return false;

  NimBLERemoteService* svc = pClient->getService("180D");
  if (!svc) { pClient->disconnect(); return false; }

  NimBLERemoteCharacteristic* chr = svc->getCharacteristic("2A37");
  if (!chr || !chr->canNotify()) { pClient->disconnect(); return false; }

  chr->subscribe(true, hrNotifyCallback);
  return true;
}

// ── Switch active sensor (call this whenever you want to swap) ────────────
void switchSensor(const char* newSensor) {
  activeSensor = newSensor;
  connectToSensor(newSensor); // watch side keeps running, no blip visible
}

// ── Setup ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  NimBLEDevice::init("ESP32 HRM");

  // Peripheral side
  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());

  NimBLEService* pHRSvc = pServer->createService("180D");
  pHRChar = pHRSvc->createCharacteristic("2A37", NIMBLE_PROPERTY::NOTIFY);
  pHRSvc->start();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID("180D");
  pAdv->start();

  // Connect to first sensor
  connectToSensor(SENSOR_A);
}

// ── Loop: keep sensor connection alive, handle switch trigger ─────────────
void loop() {
  // If sensor dropped, reconnect
  if (pClient && !pClient->isConnected()) {
    connectToSensor(activeSensor);
  }

  // Example: switch trigger via Serial (send 'A' or 'B')
  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'A') switchSensor(SENSOR_A);
    if (cmd == 'B') switchSensor(SENSOR_B);
  }

  delay(100);
}
