/*
 * espHrHub.ino - BLE Heart Rate Hub (Arduino BLE Library version)
 * 
 * Central device that connects to HR sensors (UUID 180D) and forwards
 * heart rate data to Apple Watch via BLE server advertising.
 * 
 * Uses Arduino's built-in BLE library instead of NimBLE-Arduino.
 * Key fix: pClient->connect(advertisedDevice) uses correct address type.
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEScan.h>
#include "config.h"

// ── Global State ───────────────────────────────────────────────────────────
BLEServer* pPeripheralServer = nullptr;
BLEAdvertising* pPeripheralAdvertising = nullptr;
BLECharacteristic* pHRSensorCharacteristic = nullptr;
BLE2902* pHRDescriptor2902 = nullptr;

// Sensor slots
struct SensorSlot {
  uint8_t macAddress[6];
  uint8_t lastBPM;
  unsigned long lastTimestamp;
  bool isConnected;
  BLEClient* client;
  BLEClientCallbacks* clientCallbacks;
  BLERemoteCharacteristic* hrCharacteristic;
};

static SensorSlot sensorSlots[MAX_SENSOR_SLOTS];

// Orphaned BLE clients waiting for safe deletion (avoid use-after-free with Bluedroid)
static BLEClient* orphanedClients[MAX_SENSOR_SLOTS] = {nullptr};
static BLEClientCallbacks* orphanedCallbacks[MAX_SENSOR_SLOTS] = {nullptr};
static unsigned long orphanTime[MAX_SENSOR_SLOTS] = {0};

void scheduleClientOrphan(int slotIndex, BLEClient* client, BLEClientCallbacks* callbacks) {
  if (orphanedClients[slotIndex] != nullptr) {
    // If previous orphan is still there, force-delete it (should be safe by now)
    delete orphanedClients[slotIndex];
    delete orphanedCallbacks[slotIndex];
  }
  orphanedClients[slotIndex] = client;
  orphanedCallbacks[slotIndex] = callbacks;
  orphanTime[slotIndex] = millis();
}

void cleanupOrphanedClients() {
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (orphanedClients[i] != nullptr) {
      // Wait at least 500ms after orphaning to let Bluedroid finish disconnect events
      if (millis() - orphanTime[i] >= 500 && !orphanedClients[i]->isConnected()) {
        delete orphanedClients[i];
        delete orphanedCallbacks[i];
        orphanedClients[i] = nullptr;
        orphanedCallbacks[i] = nullptr;
      }
    }
  }
}

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
         sensorSlots[slotIndex].client != nullptr;
}

// ── BLE Callback Classes ───────────────────────────────────────────────────
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) {
    watchConnected = true;
    Serial.println(F(">>> Apple Watch CONNECTED <<<"));
  }

  void onDisconnect(BLEServer* server) {
    watchConnected = false;
    Serial.println(F(">>> Apple Watch DISCONNECTED <<<"));
    BLEDevice::startAdvertising();
  }
};

// Structure to store discovered HR sensor information
struct DiscoveredDevice {
  String name;
  BLEAddress address;
  int rssi;
  bool hasHRService;
};

// Forward declarations
int scanForSensors();
bool connectToSensor(int slotIndex);
void startAdvertising();

DiscoveredDevice discoveredDevices[MAX_SENSOR_SLOTS];
int discoveredDeviceCount = 0;
// Discovered device info for auto-connect (address type only, to avoid heap/UAF races)
volatile int8_t foundAddrType = -1;

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (discoveredDeviceCount < MAX_SENSOR_SLOTS) {
        discoveredDevices[discoveredDeviceCount].name = advertisedDevice.haveName() ? String(advertisedDevice.getName().c_str()) : "(no name)";
        discoveredDevices[discoveredDeviceCount].address = advertisedDevice.getAddress();
        discoveredDevices[discoveredDeviceCount].rssi = advertisedDevice.getRSSI();
        discoveredDevices[discoveredDeviceCount].hasHRService = advertisedDevice.haveServiceUUID() &&
            advertisedDevice.getServiceUUID().equals(BLEUUID("180D"));
        
        // Auto-connect if this matches a configured sensor — just capture address type
        // CRITICAL: NimBLE stores address bytes in INVERSE order internally.
        // getNative() returns reversed bytes. macEquals() would compare reversed vs
        // original config bytes — always failing. Instead, compare two BLEAddress
        // objects (both store reversed internal bytes) via getNative().
        for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
          if (!macIsUnused(sensorSlots[i].macAddress)) {
            BLEAddress configuredAddr(sensorSlots[i].macAddress, 0);
            if (memcmp(advertisedDevice.getAddress().getNative(), configuredAddr.getNative(), 6) == 0) {
              int8_t discoveredType = advertisedDevice.getAddress().getType();
              Serial.print(F("[AUTO-CONN] Found configured sensor, type="));
              Serial.println(discoveredType);
              foundAddrType = discoveredType;
              break;
            }
          }
        }
        
        discoveredDeviceCount++;
    }
    
    Serial.print(F("   [" ));
    Serial.print(discoveredDeviceCount);
    Serial.print(F("] " ));
    if (advertisedDevice.haveName()) {
        Serial.print(advertisedDevice.getName().c_str());
    } else {
        Serial.print(F("(no name)"));
    }
    Serial.print(F("  MAC: " ));
    Serial.print(advertisedDevice.getAddress().toString().c_str());
    Serial.print(F("  type:"));
    Serial.print(advertisedDevice.getAddress().getType());
    Serial.print(F("  RSSI:"));
    Serial.print(advertisedDevice.getRSSI());
    Serial.print(F("  conn:"));
    Serial.print(advertisedDevice.isConnectable() ? "YES" : "NO");
    if (advertisedDevice.haveServiceUUID()) {
        Serial.print(F("  UUID: " ));
        Serial.print(advertisedDevice.getServiceUUID().toString().c_str());
    }
    Serial.println();
  }
};

// Static scan callback instance to avoid heap allocation on every scan
static ScanCallbacks scanCallbacks;

void hrNotificationCallback(BLERemoteCharacteristic* characteristic,
                            uint8_t* data, size_t len, bool isNotify) {
  if (len < 2) return;
  
  uint8_t flags = data[0];
  uint16_t bpm = 0;
  int idx = 1;
  
  if (flags & 0x01) {
    if (len >= 3) {
      bpm = data[idx] | (data[idx+1] << 8);
      idx += 2;
    }
  } else {
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
    pHRSensorCharacteristic->setValue(data, len);
    pHRSensorCharacteristic->notify();
    Serial.println(F("[HR] Forwarded to Apple Watch"));
  }
}

class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    Serial.println(F(">>> HR Sensor connected <<<"));
  }
  
  void onDisconnect(BLEClient* pclient) {
    Serial.println(F(">>> HR Sensor disconnected <<<"));
    // Mark the corresponding slot as disconnected so reconnection logic kicks in
    for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
      if (sensorSlots[i].client == pclient) {
        sensorSlots[i].isConnected = false;
        sensorSlots[i].hrCharacteristic = nullptr;  // prevent stale pointer use
        break;
      }
    }
  }
};

// ── Sensor Connection Functions ─────────────────────────────────────────────
bool connectToSensor(int slotIndex) {
  if (slotIndex < 0 || slotIndex >= MAX_SENSOR_SLOTS) return false;
  // Reset stale discovered type from a previous scan so we don't carry it over
  foundAddrType = -1;
  if (macIsUnused(sensorSlots[slotIndex].macAddress)) {
    Serial.print("Cannot connect: empty MAC at slot ");
    Serial.println(slotIndex);
    return false;
  }
  
  SensorSlot* slot = &sensorSlots[slotIndex];

  // If an old client exists, safely replace it rather than reusing (avoids stale
  // service cache since clearServices() is private in the Arduino BLE library).
  if (slot->client != nullptr) {
    if (slot->client->isConnected()) {
      if (slot->isConnected && slot->hrCharacteristic != nullptr) {
        return true;
      }
      Serial.println(F("[CONN] Disconnecting stale client..."));
      slot->client->disconnect();
      slot->isConnected = false;
      slot->hrCharacteristic = nullptr;
      return false;
    }
    // Disconnected — orphan for deferred cleanup, then create a fresh client
    scheduleClientOrphan(slotIndex, slot->client, slot->clientCallbacks);
    slot->client = nullptr;
    slot->clientCallbacks = nullptr;
    slot->hrCharacteristic = nullptr;
    slot->isConnected = false;
  }

  slot->client = BLEDevice::createClient();
  slot->clientCallbacks = new ClientCallbacks();
  slot->client->setClientCallbacks(slot->clientCallbacks);

  Serial.print("[CONN] Connecting to slot ");
  Serial.print(slotIndex);
  Serial.print(" MAC: ");
  for (int j = 0; j < 6; j++) {
    if (slot->macAddress[j] < 0x10) Serial.print("0");
    Serial.print(slot->macAddress[j], HEX);
    if (j < 5) Serial.print(":");
  }
  Serial.println();

  bool connected = false;
  uint8_t typesToTry[6];
  int typeCount = 0;

  // If we discovered a type from scan, try it first
  if (foundAddrType >= 0 && foundAddrType <= 3) {
    typesToTry[typeCount++] = foundAddrType;
  }
  // Always try standard types: PUBLIC (0), RANDOM (1), PUBLIC_ID (2), RANDOM_ID (3), AUTO (0xFF)
  uint8_t fallbackTypes[] = {0, 1, 2, 3, 0xFF};
  for (uint8_t ft : fallbackTypes) {
    // Skip if already added (avoid duplicates)
    bool alreadyAdded = false;
    for (int k = 0; k < typeCount; k++) {
      if (typesToTry[k] == ft) { alreadyAdded = true; break; }
    }
    if (!alreadyAdded && typeCount < 6) {
      typesToTry[typeCount++] = ft;
    }
  }

  for (int t = 0; t < typeCount && !connected; t++) {
    uint8_t type = typesToTry[t];
    const char* typeName;
    switch (type) {
      case 0:   typeName = "PUBLIC(0)";      break;
      case 1:   typeName = "RANDOM(1)";      break;
      case 2:   typeName = "PUBLIC_ID(2)";   break;
      case 3:   typeName = "RANDOM_ID(3)";   break;
      case 0xFF: typeName = "AUTO(0xFF)";    break;
      default:  typeName = "UNKNOWN";        break;
    }
    Serial.print(F("[CONN] Trying type="));
    Serial.print(typeName);
    Serial.print(F("..."));

    BLEAddress address(slot->macAddress, (type == 0xFF) ? 0 : type);
    if (slot->client->connect(address, type, 5000)) {
      Serial.println(F(" OK"));
      connected = true;
    } else {
      Serial.println(F(" FAILED"));
    }
  }

  if (!connected) {
    Serial.println(F("[CONN] All address types exhausted"));
    slot->isConnected = false;
    slot->hrCharacteristic = nullptr;
    return false;
  }

  // Clear discovered address type after successful connection
  foundAddrType = -1;
  
  BLERemoteService* svc = slot->client->getService(BLEUUID("180D"));
  if (!svc) {
    Serial.println("  Service 180D not found, disconnecting");
    slot->client->disconnect();
    slot->isConnected = false;
    slot->hrCharacteristic = nullptr;
    return false;
  }

  BLERemoteCharacteristic* chr = svc->getCharacteristic(BLEUUID("2A37"));
  if (!chr || !chr->canNotify()) {
    Serial.println("  Characteristic 2A37 not found or no notify, disconnecting");
    slot->client->disconnect();
    slot->isConnected = false;
    slot->hrCharacteristic = nullptr;
    return false;
  }
  
  chr->registerForNotify(hrNotificationCallback);
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
    slot->isConnected = false;
    slot->hrCharacteristic = nullptr;
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
      Serial.println(F("c/C - Force reconnect slot 0"));
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
  BLEDevice::init("espHRhub");
  
  pPeripheralServer = BLEDevice::createServer();
  pPeripheralServer->setCallbacks(new ServerCallbacks());
  
  BLEService* hrService = pPeripheralServer->createService(BLEUUID("180D"));
  pHRSensorCharacteristic = hrService->createCharacteristic(
    BLEUUID("2A37"),
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  
  // Add CCCD descriptor for notifications
  pHRDescriptor2902 = new BLE2902();
  pHRDescriptor2902->setNotifications(true);
  pHRSensorCharacteristic->addDescriptor(pHRDescriptor2902);
  
  // Add Body Sensor Location characteristic (2A38)
  uint8_t bsl = 2; // wrist
  BLECharacteristic* bslChar = hrService->createCharacteristic(
    BLEUUID("2A38"),
    BLECharacteristic::PROPERTY_READ
  );
  bslChar->setValue(&bsl, 1);
  
  hrService->start();
  
  pPeripheralAdvertising = BLEDevice::getAdvertising();
}

void startAdvertising() {
  if (pPeripheralAdvertising) {
    pPeripheralAdvertising->addServiceUUID(BLEUUID("180D"));
    pPeripheralAdvertising->setScanResponse(true);
    pPeripheralAdvertising->setMinInterval(32);
    pPeripheralAdvertising->setMaxInterval(80);
    pPeripheralAdvertising->setMinPreferred(0x06);
    pPeripheralAdvertising->setMaxPreferred(0x12);
    BLEDevice::startAdvertising();
    Serial.println(F("BLE advertising started as 'espHRhub' (HR sensor)"));
  }
}

int scanForSensors() {
  // Reset discovered devices array
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    discoveredDevices[i].name = "";
    discoveredDevices[i].address = BLEAddress((uint8_t[]){0,0,0,0,0,0}, 0);
    discoveredDevices[i].rssi = 0;
    discoveredDevices[i].hasHRService = false;
  }
  
  // Reset discovered address type before each scan
  foundAddrType = -1;
  
  Serial.println("Scanning for nearby BLE devices...");
  Serial.print("Scan duration: ");
  Serial.print(SCAN_DURATION_MS);
  Serial.println(" ms");
  Serial.print("Max sensor slots: ");
  Serial.println(MAX_SENSOR_SLOTS);
  
  // Must stop advertising to scan
  if (pPeripheralAdvertising) {
    BLEDevice::stopAdvertising();
    delay(100);
  }
  
  BLEScan* pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(&scanCallbacks);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  
  Serial.println(F("[SCAN] Starting BLE discovery..."));
  pScan->start(SCAN_DURATION_MS / 1000, false);
  
  Serial.println("Scan complete.");
  Serial.print("Found ");
  int count = discoveredDeviceCount;
  Serial.print(count);
  Serial.println(" devices.");
  
  discoveredDeviceCount = 0;
  
  // Resume advertising
  startAdvertising();
  
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
    sensorSlots[i].clientCallbacks = nullptr;
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

  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (!macIsUnused(sensorSlots[i].macAddress)) {
      sensorSlots[i].client = BLEDevice::createClient();
      sensorSlots[i].clientCallbacks = new ClientCallbacks();
      sensorSlots[i].client->setClientCallbacks(sensorSlots[i].clientCallbacks);
    }
  }

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
    Serial.println(F("\n[BOOT button] Scanning for HR sensors..."));
    ledMode = LED_MODE_SCANNING;
    int foundCount = scanForSensors();
    
    // If we found HR sensors, auto-connect to the first one
    bool connected = false;
    for (int i = 0; i < foundCount; i++) {
      if (discoveredDevices[i].address.toString() != "00:00:00:00:00:00" && discoveredDevices[i].hasHRService) {
        Serial.print("[BOOT] Found HR device: ");
        Serial.print(discoveredDevices[i].name.length() > 0 ? discoveredDevices[i].name.c_str() : "(no name)");
        Serial.print(" at MAC ");
        Serial.println(discoveredDevices[i].address.toString().c_str());
        
        // Only overwrite slot 0 if the configured sensor is not already connected
        if (!macIsConnected(0)) {
          const uint8_t* m = discoveredDevices[i].address.getNative();
          memcpy(sensorSlots[0].macAddress, m, 6);
          Serial.println("[BOOT] Auto-stored in slot 0");
          
          if (connectToSensor(0)) {
            activeSensorIndex = 0;
            connected = true;
            Serial.println("[BOOT] Connected to HR sensor!");
            break;
          } else {
            Serial.println("[BOOT] Failed to connect");
          }
        } else {
          Serial.println("[BOOT] Slot 0 already connected, skipping");
        }
      }
    }
    
    if (!connected) {
      Serial.println("[BOOT] No new HR sensors connected.");
    }
    
    if (activeSensorIndex != -1 && macIsConnected(activeSensorIndex)) {
      ledMode = LED_MODE_CONNECTED;
    } else {
      ledMode = LED_MODE_OFF;
    }
  }
  
  cleanupOrphanedClients();
  checkSensorReconnections();
  updateLED();

  delay(50);
}
