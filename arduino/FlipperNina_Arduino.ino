// Scans BLE and WiFi in turns (the NINA module can't do both at once) and
// streams the results to the Flipper Zero over Serial1 (D0 = RX, D1 = TX).
//
// Protocol, one tab-separated line per record:
//   B <addr> <rssi> <name> <vendor> <make> <model> <os>   BLE device, os = I (iOS), A (Android) or empty
//   W <bssid> <rssi> <ssid> <security> <channel>          WiFi network
//   E B | E W                                             end of a BLE / WiFi round

#include <ArduinoBLE.h>
#include <WiFiNINA.h>

#define FLIPPER Serial1

const unsigned long FLIPPER_BAUD = 115200;
const unsigned long BLE_SCAN_MS = 5000;
const unsigned long RESCAN_MS = 1500;
const unsigned long CONNECT_MS = 3000;
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

Device devices[MAX_DEVICES];

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

// Keeps printable ASCII only so fields never break the line/tab protocol.
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
    } else if (!d.seen && (!slot || (slot->used && d.lastSeen < slot->lastSeen))) {
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

// Google Nearby / Quick Share and Exposure Notification. Fast Pair (FE2C) is left
// out because headphones advertise it, not phones.
bool isAndroidUuid(uint16_t uuid) {
  return uuid == 0xFEF3 || uuid == 0xFC12 || uuid == 0xFD6F;
}

// Guesses the phone OS from the raw advertisement: 'I' iOS, 'A' Android, 0 unknown.
char detectPhoneOs(BLEDevice& peripheral) {
  uint8_t adv[64];
  int len = peripheral.advertisementData(adv, sizeof(adv));

  for (int i = 0; i + 1 < len;) {
    int fieldLen = adv[i];
    if (fieldLen == 0 || i + 1 + fieldLen > len) break;
    uint8_t type = adv[i + 1];
    const uint8_t* data = &adv[i + 2];
    int dataLen = fieldLen - 1;

    // Apple manufacturer data; Continuity types from iPhones/iPads, not AirPods (0x07) or Find My tags (0x12).
    if (type == 0xFF && dataLen >= 3 && data[0] == 0x4C && data[1] == 0x00) {
      uint8_t appleType = data[2];
      if (appleType == 0x10 || appleType == 0x0F || appleType == 0x0C || appleType == 0x05) return 'I';
    }
    // Complete/incomplete 16-bit service UUID lists
    if (type == 0x02 || type == 0x03) {
      for (int j = 0; j + 1 < dataLen; j += 2) {
        if (isAndroidUuid(data[j] | (data[j + 1] << 8))) return 'A';
      }
    }
    // 16-bit service data
    if (type == 0x16 && dataLen >= 2 && isAndroidUuid(data[0] | (data[1] << 8))) return 'A';

    i += 1 + fieldLen;
  }
  return 0;
}

String readStringChar(BLEDevice& peripheral, const char* uuid) {
  BLECharacteristic c = peripheral.characteristic(uuid);
  if (!c || !c.canRead() || !c.read()) return "";

  String s;
  int n = c.valueLength();
  const uint8_t* v = c.value();
  for (int i = 0; i < n; i++) s += (char)v[i];
  return clean(s);
}

void sendBle(const Device& d) {
  FLIPPER.print("B\t");
  FLIPPER.print(d.address);
  FLIPPER.print('\t');
  FLIPPER.print(d.rssi);
  FLIPPER.print('\t');
  FLIPPER.print(d.name);
  FLIPPER.print('\t');
  FLIPPER.print(d.vendor);
  FLIPPER.print('\t');
  FLIPPER.print(d.make);
  FLIPPER.print('\t');
  FLIPPER.print(d.model);
  FLIPPER.print('\t');
  if (d.os) FLIPPER.print(d.os);
  FLIPPER.print('\n');
}

// Connects to the device and reads make/model from the Device Information Service.
void readDeviceInfo(Device& d) {
  d.infoTried = true;

  // connect() needs a fresh advertisement, so find the device again.
  BLEDevice peripheral;
  bool found = false;
  BLE.scan();
  unsigned long start = millis();
  while (millis() - start < RESCAN_MS) {
    peripheral = BLE.available();
    if (peripheral && peripheral.address() == d.address) {
      found = true;
      break;
    }
  }
  BLE.stopScan();
  if (!found || !peripheral.connect()) return;

  unsigned long t = millis();
  while (!peripheral.connected() && millis() - t < CONNECT_MS) delay(10);
  if (peripheral.connected() && peripheral.discoverAttributes()) {
    // 180A Device Information: 2A29 manufacturer name, 2A24 model number
    d.make = readStringChar(peripheral, "2a29");
    d.model = readStringChar(peripheral, "2a24");
  }
  peripheral.disconnect();
  delay(100);
}

void bleRound() {
  if (!BLE.begin()) {
    Serial.println("BLE begin failed");
    delay(1000);
    return;
  }

  for (int i = 0; i < MAX_DEVICES; i++) devices[i].seen = false;

  BLE.scan();
  unsigned long start = millis();
  while (millis() - start < BLE_SCAN_MS) {
    BLEDevice peripheral = BLE.available();
    if (!peripheral) continue;

    Device* d = findOrAdd(peripheral.address());
    if (!d) continue;
    d->seen = true;
    d->lastSeen = millis();
    d->rssi = peripheral.rssi();

    // Keep a detected OS; phones don't send the telltale data in every advertisement.
    char os = detectPhoneOs(peripheral);
    if (os) d->os = os;

    if (peripheral.hasLocalName()) {
      String name = clean(peripheral.localName());
      if (name.length()) d->name = name;
    }
    if (peripheral.hasManufacturerData()) {
      uint8_t data[32];
      int n = peripheral.manufacturerData(data, sizeof(data));
      if (n >= 2) {
        const char* vendor = companyName(data[0] | (data[1] << 8));
        if (vendor) d->vendor = vendor;
      }
    }
  }
  BLE.stopScan();

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

  FLIPPER.print("E\tB\n");
  BLE.end();
}

const char* securityName(uint8_t type) {
  switch (type) {
    case ENC_TYPE_NONE: return "Open";
    case ENC_TYPE_WEP: return "WEP";
    case ENC_TYPE_TKIP: return "WPA";
    case ENC_TYPE_CCMP: return "WPA2";
    case ENC_TYPE_AUTO: return "Auto";
    default: return "?";
  }
}

void wifiRound() {
  int count = WiFi.scanNetworks();
  if (count < 0) {
    Serial.println("WiFi scan failed");
    WiFi.end();
    return;
  }

  for (int i = 0; i < count; i++) {
    uint8_t b[6];
    WiFi.BSSID(i, b);
    // WiFiNINA returns the BSSID in reverse byte order.
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
             b[5], b[4], b[3], b[2], b[1], b[0]);

    FLIPPER.print("W\t");
    FLIPPER.print(bssid);
    FLIPPER.print('\t');
    FLIPPER.print(WiFi.RSSI(i));
    FLIPPER.print('\t');
    FLIPPER.print(clean(WiFi.SSID(i)));
    FLIPPER.print('\t');
    FLIPPER.print(securityName(WiFi.encryptionType(i)));
    FLIPPER.print('\t');
    FLIPPER.print(WiFi.channel(i));
    FLIPPER.print('\n');
  }

  FLIPPER.print("E\tW\n");
  WiFi.end();
}

void setup() {
  // USB serial is for debugging only; don't wait for it, the Flipper powers the board.
  Serial.begin(115200);
  FLIPPER.begin(FLIPPER_BAUD);
}

void loop() {
  bleRound();
  wifiRound();
}
