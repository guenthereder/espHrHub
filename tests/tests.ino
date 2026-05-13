// Debug sketch: Test Arduino BLE library connection to TICKR FIT
// Requires TICKR FIT to be powered on and nearby

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEClient.h>

static const uint8_t TICKR_MAC[6] = {0x12, 0x70, 0xFE, 0xC3, 0x13, 0xF0};

// Forward declaration
class MyClientCallbacks;

void printMAC(const uint8_t* mac) {
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 0x10) Serial.print("0");
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(":");
  }
}

class MyScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    BLEAddress addr = dev.getAddress();
    Serial.print("  [SCAN] ");
    if (dev.haveName()) {
      Serial.print(dev.getName().c_str());
    } else {
      Serial.print("(no name)");
    }
    Serial.print("  MAC(type=");
    Serial.print(addr.getType());
    Serial.print("): ");
    Serial.print(addr.toString().c_str());
    Serial.print("  RSSI:");
    Serial.print(dev.getRSSI());
    Serial.print("  conn:");
    Serial.print(dev.isConnectable() ? "YES" : "NO");
    if (dev.haveServiceUUID()) {
      Serial.print("  svc:");
      Serial.print(dev.getServiceUUID().toString().c_str());
    }
    Serial.println();

    // Check if this is our TICKR
    bool match = true;
    const uint8_t* native = addr.getNative();
    for (int i = 0; i < 6; i++) {
      if (native[i] != TICKR_MAC[i]) { match = false; break; }
    }
    if (match) {
      Serial.println("  [SCAN] >>> TICKR FIT FOUND <<<");
      foundDevice = new BLEAdvertisedDevice(dev);
      foundAddrType = addr.getType();
    }
  }
};

// Global to store discovered device
BLEAdvertisedDevice* foundDevice = nullptr;
int8_t foundAddrType = -1;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(100);
  delay(500);

  Serial.println("\n=== TICKR FIT Connection Debug ===");
  Serial.print("Target MAC: ");
  printMAC(TICKR_MAC);
  Serial.println();

  BLEDevice::init("debug_espHR");

  // Scan first
  Serial.println("\n--- Phase 1: Scan ---");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new MyScanCallbacks());
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(5, false);

  Serial.println("--- Scan complete ---");
  if (foundDevice) {
    Serial.print("Discovered type: ");
    Serial.println(foundAddrType);
  } else {
    Serial.println("!!! TICKR NOT FOUND IN SCAN !!!");
  }

  // Now try to connect using discovered device object (if found)
  Serial.println("\n--- Phase 2: Connect via discovered device ---");
  if (foundDevice) {
    BLEClient* client = BLEDevice::createClient();
    Serial.println("Connecting with discovered BLEAdvertisedDevice...");
    // Arduino BLE: connect(BLEAdvertisedDevice*) takes pointer
    if (client->connect(foundDevice)) {
      Serial.println("SUCCESS via discovered device");
      client->disconnect();
    } else {
      Serial.println("FAILED via discovered device");
    }
    delete client;
  } else {
    Serial.println("Skipped (no device found)");
  }

  // Try connect with PUBLIC type
  Serial.println("\n--- Phase 3: Connect PUBLIC (type=0) ---");
  {
    BLEClient* client = BLEDevice::createClient();
    BLEAddress addr(TICKR_MAC, 0);
    Serial.println("Connecting with type=0x00 (PUBLIC)...");
    if (client->connect(addr, 0, 5000)) {
      Serial.println("SUCCESS with PUBLIC");
      client->disconnect();
    } else {
      Serial.println("FAILED with PUBLIC");
    }
    delete client;
  }

  // Try connect with RANDOM type
  Serial.println("\n--- Phase 4: Connect RANDOM (type=1) ---");
  {
    BLEClient* client = BLEDevice::createClient();
    BLEAddress addr(TICKR_MAC, 1);
    Serial.println("Connecting with type=0x01 (RANDOM)...");
    if (client->connect(addr, 1, 5000)) {
      Serial.println("SUCCESS with RANDOM");
      client->disconnect();
    } else {
      Serial.println("FAILED with RANDOM");
    }
    delete client;
  }

  // Try connect with type=0xFF (auto from address)
  Serial.println("\n--- Phase 5: Connect AUTO (type=0xFF) ---");
  {
    BLEClient* client = BLEDevice::createClient();
    // Default address type is 0 (PUBLIC)
    BLEAddress addr(TICKR_MAC, 0);
    Serial.println("Connecting with type=0xFF (auto)...");
    if (client->connect(addr, 0xFF, 5000)) {
      Serial.println("SUCCESS with AUTO");
      client->disconnect();
    } else {
      Serial.println("FAILED with AUTO");
    }
    delete client;
  }

  // Try connect with discovered type if available
  if (foundAddrType != -1) {
    Serial.println("\n--- Phase 6: Connect with discovered type ---");
    BLEClient* client = BLEDevice::createClient();
    BLEAddress addr(TICKR_MAC, foundAddrType);
    Serial.print("Connecting with discovered type=");
    Serial.print(foundAddrType);
    Serial.println("...");
    if (client->connect(addr, foundAddrType, 5000)) {
      Serial.println("SUCCESS with discovered type");
      // Check for HR service
      BLERemoteService* svc = client->getService(BLEUUID("180D"));
      if (svc) {
        Serial.println("  Service 180D found!");
        BLERemoteCharacteristic* chr = svc->getCharacteristic(BLEUUID("2A37"));
        if (chr) {
          Serial.println("  Characteristic 2A37 found!");
        } else {
          Serial.println("  Characteristic 2A37 NOT found");
        }
      } else {
        Serial.println("  Service 180D NOT found");
      }
      client->disconnect();
    } else {
      Serial.println("FAILED with discovered type");
    }
    delete client;
  }

  Serial.println("\n=== Done ===");
  Serial.println("Check output above for which method succeeded.");
}

void loop() {
  delay(1000);
}
