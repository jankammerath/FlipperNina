#include <ArduinoBLE.h>

const unsigned long SCAN_MS = 4000;
const unsigned long CONNECT_MS = 3000;
const int MAX_DEVICES = 12;

struct Device {
  String address;
  int rssi;
  String name;
  uint16_t companyId;
  bool hasCompany;
};

Device devices[MAX_DEVICES];
int deviceCount = 0;

const char* companyName(uint16_t id) {
  switch (id) {
    case 0x004C: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x00E0: return "Google";
    case 0x0075: return "Samsung";
    case 0x038F: return "Xiaomi";
    case 0x0087: return "Garmin";
    case 0x0059: return "Nordic";
    case 0x0171: return "Amazon";
    case 0x000D: return "Texas Instruments";
    case 0x000A: return "Qualcomm / CSR";
    default: return nullptr;
  }
}

int findAddress(const String& address) {
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].address == address) return i;
  }
  return -1;
}

String readStringChar(BLEDevice& peripheral, const char* uuid) {
  BLECharacteristic c = peripheral.characteristic(uuid);
  if (!c || !c.canRead() || !c.read()) return "-";

  String s;
  int n = c.valueLength();
  const uint8_t* v = c.value();
  for (int i = 0; i < n; i++) {
    if (v[i] >= 32 && v[i] < 127) s += (char)v[i];
  }
  return s.length() ? s : "-";
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  if (!BLE.begin()) {
    Serial.println("Starting BLE failed");
    while (1);
  }
  Serial.println("BLE scanner");
}

void loop() {
  deviceCount = 0;
  Serial.println();
  Serial.println("Scanning...");
  BLE.scan();

  unsigned long start = millis();
  while (millis() - start < SCAN_MS) {
    BLEDevice peripheral = BLE.available();
    if (!peripheral) continue;

    String address = peripheral.address();
    int index = findAddress(address);
    if (index < 0) {
      if (deviceCount >= MAX_DEVICES) continue;
      index = deviceCount++;
      devices[index].address = address;
      devices[index].name = "";
      devices[index].hasCompany = false;
      devices[index].companyId = 0;
    }

    devices[index].rssi = peripheral.rssi();
    if (peripheral.hasLocalName()) devices[index].name = peripheral.localName();
    if (peripheral.hasManufacturerData()) {
      uint8_t data[32];
      int n = peripheral.manufacturerData(data, sizeof(data));
      if (n >= 2) {
        devices[index].companyId = data[0] | (data[1] << 8);
        devices[index].hasCompany = true;
      }
    }
  }
  BLE.stopScan();

  Serial.println("address              RSSI  advertised manufacturer   name");
  for (int i = 0; i < deviceCount; i++) {
    Serial.print(devices[i].address);
    Serial.print("  ");
    Serial.print(devices[i].rssi);
    Serial.print("   ");
    if (devices[i].hasCompany) {
      const char* name = companyName(devices[i].companyId);
      if (name) Serial.print(name);
      else {
        Serial.print("0x");
        if (devices[i].companyId < 0x1000) Serial.print("0");
        if (devices[i].companyId < 0x100) Serial.print("0");
        if (devices[i].companyId < 0x10) Serial.print("0");
        Serial.print(devices[i].companyId, HEX);
      }
    } else {
      Serial.print("-");
    }
    Serial.print("   ");
    Serial.println(devices[i].name.length() ? devices[i].name : "(no name)");
  }

  Serial.println();
  Serial.println("Reading Device Information Service...");
  for (int i = 0; i < deviceCount; i++) {
    BLEDevice peripheral = BLE.available();
    // Re-scan briefly so connect() has a fresh advertisement.
    BLE.scan();
    unsigned long wait = millis();
    bool found = false;
    while (millis() - wait < 1500) {
      peripheral = BLE.available();
      if (peripheral && peripheral.address() == devices[i].address) {
        found = true;
        break;
      }
    }
    BLE.stopScan();

    Serial.print(devices[i].address);
    Serial.print("  ");
    if (!found || !peripheral.connect()) {
      Serial.println("no connect");
      continue;
    }

    unsigned long t = millis();
    while (!peripheral.connected() && millis() - t < CONNECT_MS) delay(10);
    if (!peripheral.connected() || !peripheral.discoverAttributes()) {
      Serial.println("no attributes");
      peripheral.disconnect();
      continue;
    }

    // 180A Device Information: 2A29 make, 2A24 model, 2A26 firmware
    Serial.print("make=");
    Serial.print(readStringChar(peripheral, "2a29"));
    Serial.print("  model=");
    Serial.print(readStringChar(peripheral, "2a24"));
    Serial.print("  fw=");
    Serial.println(readStringChar(peripheral, "2a26"));
    peripheral.disconnect();
    delay(200);
  }
}