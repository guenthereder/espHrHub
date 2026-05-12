/*
 * espHrHub.ino - BLE Heart Rate Hub
 * 
 * Central device that connects to HR sensors (UUID 180D) and forwards
 * heart rate data to Apple Watch via BLE server advertising.
 */

#include <NimBLEDevice.h>
#include "config.h"

// ── Global State ───────────────────────────────────────────────────────────
NimBLEServer* pPeripheralServer = nullptr;
NimBLEAdvertising* pPeripheralAdvertising = nullptr;
NimBLECharacteristic* pHRSensorCharacteristic = nullptr;

// Sensor slots
struct SensorSlot {
  uint8_t macAddress[6];
  uint8_t lastBPM;
  unsigned long lastTimestamp;
  bool isConnected;
  NimBLEClient* client;
  NimBLERemoteCharacteristic* hrCharacteristic;
};

static SensorSlot sensorSlots[MAX_SENSOR_SLOTS];

// Current state
int activeSensorIndex = -1;
uint8_t lastBPMFromAnySensor = 70;
bool watchConnected = false;
int ledMode = LED_MODE_OFF;
volatile bool buttonScanRequested = false;

// Reconnection tracking
unsigned long lastReconnectAttempt[MAX_SENSOR_SLOTS] = {0};
unsigned long reconnectDelayMs[MAX_SENSOR_SLOTS] = {1000, 1000, 1000, 1000};

// ── Helper Functions ───────────────────────────────────────────────────────
bool macEquals(const uint8_t* a, const uint8_t* b) {
  for (int i = 0; i < 6; i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

bool macIsUnused(const uint8_t* mac) {
  for (int i = 0; i < 6; i++) {
    if (mac[i] != 0) return false;
  }
  return true;
}

bool macIsConnected(int slotIndex) {
  if (slotIndex < 0 || slotIndex >= MAX_SENSOR_SLOTS) return false;
  return sensorSlots[slotIndex].isConnected && 
         sensorSlots[slotIndex].client != nullptr &&
         sensorSlots[slotIndex].client->isConnected();
}

// ── BLE Callback Classes ───────────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) {
    watchConnected = true;
    Serial.println(F(">>> Apple Watch CONNECTED <<<"));
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo) {
    watchConnected = false;
    Serial.println(F(">>> Apple Watch DISCONNECTED <<<"));
    pPeripheralAdvertising->start();
  }
};

class HRCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, uint16_t subValue) {
    Serial.print(F("[SUBSCRIBE] Apple Watch subscribed to HR notifications, subValue="));
    Serial.println(subValue);
  }
  
  void onUnsubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    Serial.println(F("[SUBSCRIBE] Apple Watch unsubscribed from HR notifications"));
  }
};

// Structure to store discovered HR sensor information
struct DiscoveredDevice {
  String name;
  NimBLEAddress address;
  int rssi;
};

// File-global discovered devices array — avoids static member array crash
// on ESP32 where static member arrays of complex types (e.g. NimBLEAddress)
// can have incomplete type issues that cause InstrFetchProhibited panics
// on core 0 (BLE core) during NimBLE callbacks.
DiscoveredDevice discoveredDevices[MAX_SENSOR_SLOTS];
int discoveredDeviceCount = 0;

class ScanCallbacks : public NimBLEScanCallbacks {
public:
  DiscoveredDevice* devices;
  
  ScanCallbacks() : devices(discoveredDevices) {}
  
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (discoveredDeviceCount < MAX_SENSOR_SLOTS) {
        devices[discoveredDeviceCount].name = advertisedDevice->getName().c_str();
        devices[discoveredDeviceCount].address = advertisedDevice->getAddress();
        devices[discoveredDeviceCount].rssi = advertisedDevice->getRSSI();
    }
    // Print every device we find
    Serial.print(F("   [" ));
    Serial.print(discoveredDeviceCount);
    Serial.print(F("] " ));
    if (advertisedDevice->haveName()) {
        Serial.print(advertisedDevice->getName().c_str());
    } else {
        Serial.print(F("(no name)"));
    }
    Serial.print(F("  MAC: " ));
    const uint8_t* m = advertisedDevice->getAddress().getVal();
    for (int j = 0; j < 6; j++) {
        if (m[j] < 0x10) Serial.print('0');
        Serial.print(m[j], HEX);
        if (j < 5) Serial.print(':');
    }
    Serial.print(F("  RSSI: " ));
    Serial.print(advertisedDevice->getRSSI());
    Serial.print(F("  conn:"));
    Serial.print(advertisedDevice->isConnectable() ? "YES" : "NO");
    if (advertisedDevice->haveServiceUUID()) {
        Serial.print(F("  UUID: " ));
        Serial.print(advertisedDevice->getServiceUUID().toString().c_str());
    }
    if (advertisedDevice->haveServiceData()) {
        Serial.print(F("  [has service data]"));
    }
    Serial.println();
    
    // Check if this is our configured sensor and auto-connect immediately
    if (advertisedDevice->isConnectable() && discoveredDeviceCount == 0) {
        for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
            if (!macIsUnused(sensorSlots[i].macAddress)) {
                NimBLEAddress configuredAddr(sensorSlots[i].macAddress, BLE_ADDR_PUBLIC);
                if (advertisedDevice->getAddress().equals(configuredAddr)) {
                    Serial.println(F("[AUTO-CONN] Found configured sensor during scan, connecting NOW..."));
                    // Stop scan to connect
                    NimBLEDevice::getScan()->stop();
                    // Use the advertised address directly (has correct type)
                    SensorSlot* slot = &sensorSlots[i];
                    if (slot->client != nullptr) {
                        if (slot->isConnected && slot->client->isConnected()) {
                            slot->client->disconnect();
                        }
                        NimBLEDevice::deleteClient(slot->client);
                        slot->client = nullptr;
                    }
                    slot->client = NimBLEDevice::createClient();
                    NimBLEDevice::setSecurityAuth(true, true, true);
                    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
                    if (slot->client->connect(advertisedDevice->getAddress())) {
                        NimBLERemoteService* svc = slot->client->getService("180D");
                        if (svc) {
                            NimBLERemoteCharacteristic* chr = svc->getCharacteristic("2A37");
                            if (chr && chr->canNotify()) {
                                chr->subscribe(true, hrNotificationCallback);
                                slot->hrCharacteristic = chr;
                                slot->isConnected = true;
                                activeSensorIndex = i;
                                Serial.println(F("[AUTO-CONN] Connected during scan!"));
                            }
                        }
                    } else {
                        Serial.println(F("[AUTO-CONN] Connect failed during scan"));
                        NimBLEDevice::deleteClient(slot->client);
                        slot->client = nullptr;
                    }
                    break;
                }
            }
        }
    }
    
    discoveredDeviceCount++;
}
};

// File-global — lives for the entire program lifetime so NimBLE's raw pointer
// to this callback never dangles (fixes InstrFetchProhibited panic on Core 0).
ScanCallbacks scanCallbacks;

void hrNotificationCallback(NimBLERemoteCharacteristic* characteristic, 
                            uint8_t* data, size_t len, bool isNotify) {
  if (len < 2) return;
  
  uint8_t flags = data[0];
  uint16_t bpm = 0;
  int idx = 1;
  
  if (flags & 0x01) {
    // 16-bit heart rate
    if (len >= 3) {
      bpm = data[idx] | (data[idx+1] << 8);
      idx += 2;
    }
  } else {
    // 8-bit heart rate
    bpm = data[idx];
    idx++;
  }
  
  lastBPMFromAnySensor = bpm;
  
  Serial.print(F("[HR] Received from strap: flags=0x"));
  Serial.print(flags, HEX);
  Serial.print(F(" BPM="));
  Serial.print(bpm);
  Serial.print(F(" raw=["));
  for (size_t i = 0; i < len; i++) {
    if (i > 0) Serial.print(" ");
    Serial.print(data[i], HEX);
  }
  Serial.println(F("]"));
  
  if (watchConnected && pHRSensorCharacteristic != nullptr) {
    // Forward raw packet as-is
    pHRSensorCharacteristic->setValue(data, len);
    if (pHRSensorCharacteristic->notify()) {
      Serial.println(F("[HR] Forwarded to Apple Watch"));
    } else {
      Serial.println(F("[HR] FAILED to notify Apple Watch (not subscribed?)"));
    }
  }
};

// ── Sensor Connection Functions ─────────────────────────────────────────────
bool connectToSensor(int slotIndex) {
  if (slotIndex < 0 || slotIndex >= MAX_SENSOR_SLOTS) return false;
  if (macIsUnused(sensorSlots[slotIndex].macAddress)) {
    Serial.print("Cannot connect: empty MAC at slot ");
    Serial.println(slotIndex);
    return false;
  }
  
  SensorSlot* slot = &sensorSlots[slotIndex];
  
  if (slot->client != nullptr) {
    if (slot->isConnected && slot->client->isConnected()) {
      slot->client->disconnect();
    }
    NimBLEDevice::deleteClient(slot->client);
    slot->client = nullptr;
  }
  
  Serial.print("[CONN] Connecting to MAC: ");
  for (int j = 0; j < 6; j++) {
    if (slot->macAddress[j] < 0x10) Serial.print("0");
    Serial.print(slot->macAddress[j], HEX);
    if (j < 5) Serial.print(":");
  }
  Serial.print(" type=PUBLIC");
  Serial.println();
  
  NimBLEAddress address(slot->macAddress, BLE_ADDR_PUBLIC);
  slot->client = NimBLEDevice::createClient();
  
  // Enable security (some straps require bonding)
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  
  // Increase connection timeout and set params for TICKR FIT
  slot->client->setConnectTimeout(10); // 10 seconds
  slot->client->setConnectionParams(6, 12, 0, 500); // min=7.5ms, max=15ms, latency=0, timeout=5s
  
  Serial.print("[CONN] Connecting with 10s timeout...");
  
  // Try synchronous connect first
  if (!slot->client->connect(address, true, false, true)) {
    Serial.println(" FAILED (sync)");
    
    // Try async connect and wait
    Serial.print("[CONN] Trying async connect...");
    if (slot->client->connect(address, true, true, true)) {
      Serial.println(" async started");
      // Wait up to 10 seconds for connection
      unsigned long startConn = millis();
      while (!slot->client->isConnected() && millis() - startConn < 10000) {
        delay(100);
      }
      if (!slot->client->isConnected()) {
        Serial.println("[CONN] Async connect timed out");
        NimBLEDevice::deleteClient(slot->client);
        slot->client = nullptr;
        return false;
      }
      Serial.println("[CONN] Async connect succeeded!");
    } else {
      Serial.println(" FAILED (async too)");
      NimBLEDevice::deleteClient(slot->client);
      slot->client = nullptr;
      return false;
    }
  }
  
  Serial.println(" OK");
  
  NimBLERemoteService* svc = slot->client->getService("180D");
  if (!svc) {
    Serial.println("  Service 180D not found, disconnecting");
    slot->client->disconnect();
    return false;
  }
  
  NimBLERemoteCharacteristic* chr = svc->getCharacteristic("2A37");
  if (!chr || !chr->canNotify()) {
    Serial.println("  Characteristic 2A37 not found or no notify, disconnecting");
    slot->client->disconnect();
    return false;
  }
  
  chr->subscribe(true, hrNotificationCallback);
  slot->hrCharacteristic = chr;
  slot->isConnected = true;
  
  Serial.println("  Subscribed to HR notifications");
  
  reconnectDelayMs[slotIndex] = 1000;
  
  return true;
}

bool disconnectSensor(int slotIndex) {
  if (slotIndex < 0 || slotIndex >= MAX_SENSOR_SLOTS) return false;
  
  SensorSlot* slot = &sensorSlots[slotIndex];
  if (slot->client != nullptr && slot->isConnected) {
    slot->client->disconnect();
  }
  
  return true;
}

void switchActiveSensor(int newSlotIndex) {
  if (newSlotIndex < 0 || newSlotIndex >= MAX_SENSOR_SLOTS) {
    Serial.println("Invalid slot index");
    return;
  }
  
  if (macIsUnused(sensorSlots[newSlotIndex].macAddress)) {
    Serial.print("No sensor stored in slot ");
    Serial.println(newSlotIndex);
    return;
  }
  
  if (activeSensorIndex != -1 && activeSensorIndex != newSlotIndex) {
    disconnectSensor(activeSensorIndex);
  }
  
  if (connectToSensor(newSlotIndex)) {
    activeSensorIndex = newSlotIndex;
    Serial.print("Switched to sensor at slot ");
    Serial.println(activeSensorIndex);
  } else {
    Serial.print("Failed to connect to sensor at slot ");
    Serial.println(newSlotIndex);
  }
}

// ── LED Management ────────────────────────────────────────────────────────
void updateLED() {
  static unsigned long lastLedToggle = 0;
  static bool ledState = false;
  
  switch (ledMode) {
    case LED_MODE_OFF:
      digitalWrite(LED_PIN, LOW);
      break;
      
    case LED_MODE_SCANNING:
      if (millis() - lastLedToggle >= 250) {
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        lastLedToggle = millis();
      }
      break;
      
    case LED_MODE_CONNECTING:
      if (millis() - lastLedToggle >= 50) {
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        lastLedToggle = millis();
      }
      break;
      
    case LED_MODE_CONNECTED:
      digitalWrite(LED_PIN, HIGH);
      break;
  }
}

// ── Button Handling ───────────────────────────────────────────────────────
void handleSerialInput() {
  while (Serial.available()) {
    char cmd = Serial.read();
    
    if (cmd == COMMAND_SCAN || cmd == 's' || cmd == 'S') {
      Serial.println("Scanning for HR sensors...");
      scanForSensors();
    }
    else if (cmd == COMMAND_SELECT_1 || cmd == '1') {
      switchActiveSensor(0);
    }
    else if (cmd == COMMAND_SELECT_2 || cmd == '2') {
      switchActiveSensor(1);
    }
    else if (cmd == COMMAND_SELECT_3 || cmd == '3') {
      switchActiveSensor(2);
    }
    else if (cmd == COMMAND_SELECT_4 || cmd == '4') {
      switchActiveSensor(3);
    }
    else if (cmd == 'c' || cmd == 'C') {
      Serial.println("[CMD] Force reconnect to slot 0...");
      if (connectToSensor(0)) {
        activeSensorIndex = 0;
        Serial.println("[CMD] Connected!");
      } else {
        Serial.println("[CMD] Failed");
      }
    }
    else if (cmd == COMMAND_HELP || cmd == '?') {
      Serial.println(F("===espHrHub Help==="));
      Serial.println(F("S/s - Scan for sensors"));
      Serial.println(F("1-4 - Select sensor slot 1-4"));
      Serial.println(F("?   - Show this help"));
    }
    else {
      Serial.print("Unknown command: ");
      Serial.println(cmd);
    }
  }
}

// ── Reconnection Logic ─────────────────────────────────────────────────────
void checkSensorReconnections() {
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (!macIsUnused(sensorSlots[i].macAddress) && !macIsConnected(i)) {
      unsigned long now = millis();
      
      if (now - lastReconnectAttempt[i] >= reconnectDelayMs[i]) {
        Serial.print("Attempting to reconnect to slot ");
        Serial.println(i);
        
        if (connectToSensor(i)) {
          if (activeSensorIndex == i) {
            Serial.println("Reconnect successful - sensor is now active");
          }
        } else {
          reconnectDelayMs[i] *= 2;
          if (reconnectDelayMs[i] > MAX_RECONNECT_DELAY_MS) {
            reconnectDelayMs[i] = MAX_RECONNECT_DELAY_MS;
          }
        }
        
        lastReconnectAttempt[i] = now;
      }
    }
  }
}

// ── BLE Setup Functions ────────────────────────────────────────────────────
void setupBLE() {
   NimBLEDevice::init("espHRhub");
  
  pPeripheralServer = NimBLEDevice::createServer();
  pPeripheralServer->setCallbacks(new ServerCallbacks());
  
  NimBLEService* hrService = pPeripheralServer->createService("180D");
  pHRSensorCharacteristic = hrService->createCharacteristic(
      "2A37", 
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
  pHRSensorCharacteristic->setCallbacks(new HRCharacteristicCallbacks());
  // Add Body Sensor Location characteristic (2A38) for Apple Watch compatibility
  // Values: 0=Other, 1=chest, 2=wrist, 3=thumb, 4=finger, 5=earlobe, 6=foot
  uint8_t bsl = 2; // wrist
  hrService->createCharacteristic(
    "2A38", 
    NIMBLE_PROPERTY::READ
  )->setValue(&bsl, 1);
  hrService->start();
  
  pPeripheralAdvertising = NimBLEDevice::getAdvertising();
}

void startAdvertising() {
  if (pPeripheralAdvertising) {
    pPeripheralAdvertising->addServiceUUID("180D");
    pPeripheralAdvertising->setAppearance(0x0340);         // Heart Rate Sensor
    pPeripheralAdvertising->setName("espHRhub");
    pPeripheralAdvertising->enableScanResponse(true);
    pPeripheralAdvertising->setMinInterval(32);
    pPeripheralAdvertising->setMaxInterval(80);
    pPeripheralAdvertising->start();
    Serial.println(F("BLE advertising started as 'espHRhub' (HR sensor)"));
  }
}

int scanForSensors() {
   // Reset discovered devices array
   for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
     discoveredDevices[i].name = "";
     discoveredDevices[i].address = NimBLEAddress((uint8_t[]){0,0,0,0,0,0}, BLE_ADDR_PUBLIC);
     discoveredDevices[i].rssi = 0;
   }
   
    Serial.println("Scanning for nearby BLE devices...");
    Serial.print("Scan duration: ");
    Serial.print(SCAN_DURATION_MS);
    Serial.println(" ms");
    Serial.print("Max sensor slots: ");
    Serial.println(MAX_SENSOR_SLOTS);
  
    // Must stop advertising to scan — single BLE radio can't do both
    if (pPeripheralAdvertising) {
        pPeripheralAdvertising->stop();
        delay(100);   // Let advertising fully stop
    }
  
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(&scanCallbacks);
    pScan->start(SCAN_DURATION_MS, false);
  
   Serial.println("Scan complete.");
   Serial.print("Found ");
   int count = discoveredDeviceCount;
   Serial.print(count);
   Serial.println(" devices.");
  
   discoveredDeviceCount = 0;

    // Resume advertising after scan
    startAdvertising();

    // Return number of devices found so caller can auto-connect
    return count;
}

// ── Setup Function ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(100);
  
  Serial.println(F("\n=== espHrHub Starting ==="));
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.println(F("BOOT button configured on GPIO0 — press to scan for sensors"));
  
  pinMode(LED_PIN, OUTPUT);
  ledMode = LED_MODE_OFF;
  
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    sensorSlots[i].client = nullptr;
    sensorSlots[i].hrCharacteristic = nullptr;
    sensorSlots[i].isConnected = false;
    sensorSlots[i].lastBPM = 70;
    
    memcpy(sensorSlots[i].macAddress, DISCOVERED_MAC_ADDRESSES[i], 6);
    
    if (!macIsUnused(sensorSlots[i].macAddress)) {
      Serial.print("Configured sensor slot ");
      Serial.print(i);
      Serial.print(": ");
      for (int j = 0; j < 6; j++) {
        Serial.print(sensorSlots[i].macAddress[j], HEX);
        if (j < 5) Serial.print(":");
      }
      Serial.println();
    }
  }
  
  setupBLE();
  startAdvertising();
  
  Serial.println(F("\nConnecting to configured sensors..."));
  bool connectedAny = false;
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (!macIsUnused(sensorSlots[i].macAddress)) {
      if (connectToSensor(i)) {
        connectedAny = true;
        if (activeSensorIndex == -1) {
          activeSensorIndex = i;
        }
      }
    }
  }
  
  if (!connectedAny) {
    Serial.println(F("No sensors configured or available. Press button to scan."));
  } else {
    Serial.print(F("Active sensor: slot "));
    Serial.println(activeSensorIndex);
  }
  
  if (activeSensorIndex != -1 && macIsConnected(activeSensorIndex)) {
    ledMode = LED_MODE_CONNECTED;
  }
  
  Serial.println(F("\n=== Setup Complete ==="));
  Serial.println(F(""));
  Serial.println(F("Ready. Apple Watch should see 'espHRhub' in HR sensors list."));
  Serial.println(F("Press BOOT button (GPIO0) to scan for external HR straps."));
}

// ── Main Loop ──────────────────────────────────────────────────────────────
void loop() {
  // Poll BOOT button (GPIO0) with debounce
  static unsigned long lastBtnCheck = 0;
  static bool lastBtnState = HIGH;
  if (millis() - lastBtnCheck > 50) {
    bool btnState = digitalRead(BUTTON_PIN);
    if (btnState == LOW && lastBtnState == HIGH) {
      buttonScanRequested = true;
    }
    lastBtnState = btnState;
    lastBtnCheck = millis();
  }

  handleSerialInput();
  
  if (buttonScanRequested) {
    buttonScanRequested = false;
    Serial.println("\n[BOOT button] Scanning for HR sensors...");
    ledMode = LED_MODE_SCANNING;
    int foundCount = scanForSensors();
    
    // If we found sensors, auto-connect to the first one with HR service (180D)
    bool connected = false;
    for (int i = 0; i < foundCount; i++) {
      // Check if this device advertises HR service 180D
      // We can't check here easily, so just try connecting to all non-empty slots
      // The scan callback would have stored devices
      // Actually, let's check the discoveredDevices array for any device
      if (discoveredDevices[i].address.toString() != "00:00:00:00:00:00") {
        Serial.print("[BOOT] Found device: ");
        Serial.print(discoveredDevices[i].name.length() > 0 ? discoveredDevices[i].name.c_str() : "(no name)");
        Serial.print(" at MAC ");
        const uint8_t* m = discoveredDevices[i].address.getVal();
        for (int j = 0; j < 6; j++) {
          if (m[j] < 0x10) Serial.print("0");
          Serial.print(m[j], HEX);
          if (j < 5) Serial.print(":");
        }
        Serial.println();
        
        // Store MAC in slot 0
        memcpy(sensorSlots[0].macAddress, discoveredDevices[i].address.getVal(), 6);
        Serial.println("[BOOT] Auto-stored in slot 0");
        
        if (connectToSensor(0)) {
          activeSensorIndex = 0;
          connected = true;
          Serial.println("[BOOT] Connected to HR sensor!");
          break;
        } else {
          Serial.println("[BOOT] Failed to connect");
        }
      }
    }
    
    if (!connected) {
      Serial.println("[BOOT] No HR sensors connected. Will retry via auto-reconnect.");
    }
    
    if (activeSensorIndex != -1 && macIsConnected(activeSensorIndex)) {
      ledMode = LED_MODE_CONNECTED;
    } else {
      ledMode = LED_MODE_OFF;
    }
   }
  
  checkSensorReconnections();
  updateLED();
  
  delay(50);
}
