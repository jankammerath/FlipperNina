#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>

// Use HardwareSerial 2 for communicating with Flipper Zero
#define FLIPPER Serial2
const int FLIPPER_RX_PIN = 16; // Connect to Flipper TX (GPIO 13 / USART TX)
const int FLIPPER_TX_PIN = 17; // Connect to Flipper RX (GPIO 14 / USART RX)

const unsigned long FLIPPER_BAUD = 115200;
const uint32_t BLE_SCAN_TIME_SEC = 5;
const unsigned long CONNECT_TIMEOUT_MS = 3000;
const int MAX_DEVICES = 32;
const int MAX_INFO_READS_PER_ROUND = 2;
const unsigned int MAX_FIELD_LEN = 32;

struct Device {
  bool used;
  bool seen;
  bool infoTried;
  unsigned long lastSeen;
  String address;
  int rssi;
  String name;
  String vendor;
  String make;
  String model;
  char os;
};

static Device devices[MAX_DEVICES];
BLEScan* pBLEScan = nullptr;

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

String clean(const String& in) {
  String out;
  for (unsigned int i = 0; i < in.length() && out.length() < MAX_FIELD_LEN; i++) {
    char c = in[i];
    if (c >= 32 && c < 127) out += c;
  }
  out.trim();
  return out;
}

Device* findOrAdd(const String& address) {
  Device* slot = nullptr;
  for (int i = 0; i < MAX_DEVICES; i++) {
    Device& d = devices[i];
    if (d.used && d.address == address) return &d;
    if (!d.used) {
      if (!slot || slot->used) slot = &d;
    } else if (!d.seen && (!slot || (slot->used && (long)(slot->lastSeen - d.lastSeen) > 0))) {
      slot = &d;
    }
  }
  if (!slot) return nullptr;

  slot->used = true;
  slot->seen = false;
  slot->infoTried = false;
  slot->address = address;
  slot->rssi = 0;
  slot->name = "";
  slot->vendor = "";
  slot->make = "";
  slot->model = "";
  slot->os = 0;
  return slot;
}

bool isAndroidUuid(uint16_t uuid) {
  return uuid == 0xFEF3 || uuid == 0xFC12 || uuid == 0xFD6F;
}

char detectPhoneOs(BLEAdvertisedDevice& advertisedDevice) {
  uint8_t* payload = advertisedDevice.getPayload();
  size_t totalLen = advertisedDevice.getPayloadLength();

  for (size_t i = 0; i + 1 < totalLen;) {
    uint8_t fieldLen = payload[i];
    if (fieldLen == 0 || i + 1 + fieldLen > totalLen) break;
    uint8_t type = payload[i + 1];
    uint8_t* data = &payload[i + 2];
    int dataLen = fieldLen - 1;

    // Apple Continuity
    if (type == 0xFF && dataLen >= 3 && data[0] == 0x4C && data[1] == 0x00) {
      uint8_t appleType = data[2];
      if (appleType == 0x10 || appleType == 0x0F || appleType == 0x0C || appleType == 0x05) {
        return 'I';
      }
    }
    // 16-bit Service UUIDs
    if (type == 0x02 || type == 0x03) {
      for (int j = 0; j + 1 < dataLen; j += 2) {
        uint16_t u = data[j] | (data[j + 1] << 8);
        if (isAndroidUuid(u)) return 'A';
      }
    }
    // 16-bit Service Data
    if (type == 0x16 && dataLen >= 2) {
      uint16_t u = data[0] | (data[1] << 8);
      if (isAndroidUuid(u)) return 'A';
    }

    i += 1 + fieldLen;
  }
  return 0;
}

void sendLine(const String& line) {
  uint8_t sum = 0;
  for (unsigned int i = 0; i < line.length(); i++) {
    sum ^= (uint8_t)line[i];
  }
  char tail[6];
  snprintf(tail, sizeof(tail), "*%02X\n", sum);
  FLIPPER.print(line);
  FLIPPER.print(tail);
}

void sendBle(const Device& d) {
  String line = "B\t";
  line += d.address;
  line += '\t';
  line += d.rssi;
  line += '\t';
  line += d.name;
  line += '\t';
  line += d.vendor;
  line += '\t';
  line += d.make;
  line += '\t';
  line += d.model;
  line += '\t';
  if (d.os) line += d.os;
  sendLine(line);
}

void readDeviceInfo(Device& d) {
  d.infoTried = true;

  BLEAddress addr(d.address.c_str());
  BLEClient* pClient = BLEDevice::createClient();
  if (!pClient) return;

  if (!pClient->connect(addr)) {
    delete pClient;
    return;
  }

  // Device Information Service UUID: 0x180A
  BLERemoteService* pService = pClient->getService(BLEUUID((uint16_t)0x180A));
  if (pService != nullptr) {
    // Manufacturer Name String (0x2A29)
    BLERemoteCharacteristic* pCharMake = pService->getCharacteristic(BLEUUID((uint16_t)0x2A29));
    if (pCharMake && pCharMake->canRead()) {
      d.make = clean(pCharMake->readValue());
    }

    // Model Number String (0x2A24)
    BLERemoteCharacteristic* pCharModel = pService->getCharacteristic(BLEUUID((uint16_t)0x2A24));
    if (pCharModel && pCharModel->canRead()) {
      d.model = clean(pCharModel->readValue());
    }
  }

  pClient->disconnect();
  delete pClient;
}

void bleRound() {
  for (int i = 0; i < MAX_DEVICES; i++) devices[i].seen = false;

  BLEScanResults* foundDevices = pBLEScan->start(BLE_SCAN_TIME_SEC, false);
  int count = foundDevices->getCount();

  for (int i = 0; i < count; i++) {
    BLEAdvertisedDevice adv = foundDevices->getDevice(i);
    String addrStr = adv.getAddress().toString();
    addrStr.toUpperCase();

    Device* d = findOrAdd(addrStr);
    if (!d) continue;

    d->seen = true;
    d->lastSeen = millis();
    d->rssi = adv.getRSSI();

    char os = detectPhoneOs(adv);
    if (os) d->os = os;

    if (adv.haveName()) {
      String name = clean(adv.getName());
      if (name.length()) d->name = name;
    }

    if (adv.haveManufacturerData()) {
      String mData = adv.getManufacturerData();
      if (mData.length() >= 2) {
        uint16_t id = (uint8_t)mData[0] | ((uint8_t)mData[1] << 8);
        const char* vendor = companyName(id);
        if (vendor) d->vendor = vendor;
      }
    }
  }

  for (int i = 0; i < MAX_DEVICES; i++) {
    if (devices[i].used && devices[i].seen) sendBle(devices[i]);
  }

  int reads = 0;
  for (int i = 0; i < MAX_DEVICES && reads < MAX_INFO_READS_PER_ROUND; i++) {
    Device& d = devices[i];
    if (!d.used || !d.seen || d.infoTried) continue;
    readDeviceInfo(d);
    reads++;
    if (d.make.length() || d.model.length()) sendBle(d);
  }

  pBLEScan->clearResults();
  sendLine("E\tB");
}

const char* securityName(wifi_auth_mode_t type) {
  switch (type) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
    default: return "?";
  }
}

void wifiRound() {
  int count = WiFi.scanNetworks();
  if (count < 0) {
    Serial.println("WiFi scan failed");
    WiFi.scanDelete();
    return;
  }

  for (int i = 0; i < count; i++) {
    String line = "W\t";
    line += WiFi.BSSIDstr(i);
    line += '\t';
    line += WiFi.RSSI(i);
    line += '\t';
    line += clean(WiFi.SSID(i));
    line += '\t';
    line += securityName(WiFi.encryptionType(i));
    line += '\t';
    line += WiFi.channel(i);
    sendLine(line);
  }

  sendLine("E\tW");
  WiFi.scanDelete();
}

void setup() {
  Serial.begin(115200);

  // Initialize Serial2 for Flipper Zero on GPIO 16 (RX) and GPIO 17 (TX)
  FLIPPER.begin(FLIPPER_BAUD, SERIAL_8N1, FLIPPER_RX_PIN, FLIPPER_TX_PIN);

  // WiFi station mode for scanning
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // BLE initialization
  BLEDevice::init("ESP32_Scanner");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
}

void loop() {
  bleRound();
  wifiRound();
}