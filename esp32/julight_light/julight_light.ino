// Julight - ESP32 light controller with BLE.
//
// Keeps the original physical selector (pins 4/5) + smooth PWM on pin 27, and
// adds a BLE peripheral so a Garmin watch can set the brightness remotely.
//
// BLE contract (must match the Connect IQ app):
//   Service : c3a1f200-9b0e-4f1a-8a01-0e1d2c3b4a59  (advertised)
//   Data    : c3a1f201-...  watch -> ESP32, WRITE   (1 byte = brightness 0..255)
//   Notify  : c3a1f202-...  ESP32 -> watch, NOTIFY  (1 byte = current brightness)
//
// Control model: "last input wins". The physical selector overrides only when it
// changes position; a BLE write overrides until the selector is moved.

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define SERVICE_UUID     "c3a1f200-9b0e-4f1a-8a01-0e1d2c3b4a59"
#define DATA_CHAR_UUID   "c3a1f201-9b0e-4f1a-8a01-0e1d2c3b4a59"
#define NOTIFY_CHAR_UUID "c3a1f202-9b0e-4f1a-8a01-0e1d2c3b4a59"
#define DEVICE_NAME      "Julight-ESP32"

const int pinLow  = 4;
const int pinHigh = 5;
const int ledPin  = 27;

volatile int currentBrightness = 0;
volatile int targetBrightness  = 0;

int  lastSwitchTarget = -1;     // edge detection for the physical selector
bool deviceConnected  = false;

BLECharacteristic* notifyChar = nullptr;

static int clampByte(int v) {
  if (v < 0)   { return 0; }
  if (v > 255) { return 255; }
  return v;
}

// OFF / MEDIUM / FULL from the physical selector (unchanged logic).
int readSwitchTarget() {
  bool lowState  = digitalRead(pinLow);
  bool highState = digitalRead(pinHigh);
  if (lowState == LOW)  { return 0; }     // OFF
  if (highState == LOW) { return 255; }   // FULL
  return 128;                             // MEDIUM
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    deviceConnected = true;
  }
  void onDisconnect(BLEServer* s) override {
    deviceConnected = false;
    BLEDevice::startAdvertising();        // allow the watch to reconnect
  }
};

class DataCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    uint8_t* data = c->getData();
    size_t   len  = c->getLength();
    if (len >= 1) {
      targetBrightness = clampByte(data[0]);   // first byte = brightness 0..255
    }
  }
};

void setup() {
  pinMode(pinLow,  INPUT_PULLUP);
  pinMode(pinHigh, INPUT_PULLUP);
  pinMode(ledPin,  OUTPUT);

  targetBrightness = readSwitchTarget();
  lastSwitchTarget = targetBrightness;

  BLEDevice::init(DEVICE_NAME);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* service = server->createService(SERVICE_UUID);

  BLECharacteristic* dataChar = service->createCharacteristic(
      DATA_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
  dataChar->setCallbacks(new DataCallbacks());

  notifyChar = service->createCharacteristic(
      NOTIFY_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  notifyChar->addDescriptor(new BLE2902());

  service->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);       // so the watch can match by UUID
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

void loop() {
  // Physical selector overrides only when it actually changes (last input wins).
  int sw = readSwitchTarget();
  if (sw != lastSwitchTarget) {
    targetBrightness = sw;
    lastSwitchTarget = sw;
  }

  // Smooth transition.
  if (currentBrightness < targetBrightness) {
    currentBrightness++;
  } else if (currentBrightness > targetBrightness) {
    currentBrightness--;
  }
  analogWrite(ledPin, currentBrightness);

  // Tell the watch the new level once a transition settles.
  static int lastNotified = -1;
  if (deviceConnected && notifyChar != nullptr &&
      currentBrightness == targetBrightness && targetBrightness != lastNotified) {
    uint8_t b = (uint8_t) currentBrightness;
    notifyChar->setValue(&b, 1);
    notifyChar->notify();
    lastNotified = targetBrightness;
  }

  delay(5);
}
