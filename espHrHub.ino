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
  }

  void onDisconnect(NimBLEServer* server) {
    watchConnected = false;
    pPeripheralAdvertising->start();
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
    if (advertisedDevice->haveServiceUUID()) {
        Serial.print(F("  UUID: " ));
        Serial.print(advertisedDevice->getServiceUUID().toString().c_str());
    }
    if (advertisedDevice->haveServiceData()) {
        Serial.print(F("  [has service data]"));
    }
    Serial.println();
    discoveredDeviceCount++;
}
};

// File-global — lives for the entire program lifetime so NimBLE's raw pointer
// to this callback never dangles (fixes InstrFetchProhibited panic on Core 0).
ScanCallbacks scanCallbacks;

void hrNotificationCallback(NimBLERemoteCharacteristic* characteristic, 
                            uint8_t* data, size_t len, bool isNotify) {
  lastBPMFromAnySensor = data[1];
  if (watchConnected && pHRSensorCharacteristic != nullptr) {
    pHRSensorCharacteristic->setValue(data, len);
    pHRSensorCharacteristic->notify();
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
  
  NimBLEAddress address(slot->macAddress, BLE_ADDR_PUBLIC);
  slot->client = NimBLEDevice::createClient();
  
  Serial.print("Connecting to sensor at slot ");
  Serial.print(slotIndex);
  Serial.print("...");
  
  if (!slot->client->connect(address)) {
    Serial.println(" FAILED");
    NimBLEDevice::deleteClient(slot->client);
    slot->client = nullptr;
    return false;
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
    NIMBLE_PROPERTY::NOTIFY
   );
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

void scanForSensors() {
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
    pScan->start(SCAN_DURATION_MS / 1000.0, false);
  
   Serial.println("Scan complete.");
   Serial.print("Found ");
   int count = discoveredDeviceCount;
   Serial.print(count);
   Serial.println(" devices.");
  
   discoveredDeviceCount = 0;

   // Resume advertising after scan
   startAdvertising();

   for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (!macIsUnused(sensorSlots[i].macAddress)) {
        Serial.print(" Slot ");
        Serial.print(i);
        Serial.print(": ");
        for (int j = 0; j < 6; j++) {
          if (sensorSlots[i].macAddress[j] < 16) Serial.print("0");
          Serial.print(sensorSlots[i].macAddress[j], HEX);
        if (j < 5) Serial.print(":");
      }
      
      if (!macIsConnected(i) && reconnectDelayMs[i] == 1000) {
        Serial.print(" [connecting]");
        connectToSensor(i);
      }
      Serial.println();
    }
    }
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
    Serial.println(F("\n[BOOT button] Initiating sensor scan..."));
    ledMode = LED_MODE_SCANNING;
    scanForSensors();
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
