// Julight - ESP32 light controller with BLE.
//
// Keeps the original physical selector (pins 4/5) + smooth PWM light on GPIO 27,
// drives a constant-speed motor (L9110S), counts revolutions from a microswitch
// encoder, and exposes a BLE peripheral for a Garmin watch.
//
// BLE contract (must match the Connect IQ app):
//   Service : c3a1f200-9b0e-4f1a-8a01-0e1d2c3b4a59  (advertised)
//   Data    : c3a1f201-...  watch -> ESP32, WRITE   (telemetry frame, see below)
//   Notify  : c3a1f202-...  ESP32 -> watch, NOTIFY  (1 byte = current brightness)
//
//   Data frame: [HR][N][ N x (accelX,Y,Z int16 big-endian, milli-g) ].
//   Byte 0 (HR) currently drives brightness.
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

// --- Pin map ---------------------------------------------------------------
//   Toggle switch (COM->GND) : pins 4 / 5      (OFF / MED / FULL selector)
//   Light (IRLZ44N gate)     : GPIO 27         (PWM brightness)
//   Motor driver L9110S      : GPIO 25 / 26    (constant-speed DC motor)
//   Microswitch (N.O.->GND)  : GPIO 33         (1 pulse per revolution = encoder)
const int pinLow     = 4;
const int pinHigh    = 5;
const int ledPin     = 27;
const int pinMotorA  = 25;      // L9110S input A
const int pinMotorB  = 26;      // L9110S input B
const int pinEncoder = 33;      // microswitch, normally open to GND

const int MOTOR_SPEED = 200;    // constant drive PWM, 0..255 (one direction)

// Ignore contact bounce shorter than this; also caps max detectable RPM
// (3 ms -> up to ~20000 rpm, far beyond a mechanical microswitch).
const uint32_t REV_DEBOUNCE_US = 3000;

volatile int currentBrightness = 0;
volatile int targetBrightness  = 0;

// Encoder state, updated from the microswitch interrupt.
volatile uint32_t revCount    = 0;   // total revolutions counted
volatile uint32_t revPeriodUs = 0;   // micros between the last two revolutions
volatile uint32_t lastEdgeUs  = 0;   // timestamp of the last accepted edge

int  lastSwitchTarget = -1;     // edge detection for the physical selector
bool deviceConnected  = false;

BLECharacteristic* notifyChar = nullptr;

// One microswitch closure = one revolution. Debounced in-handler.
void IRAM_ATTR onRevolution() {
  uint32_t now = micros();
  uint32_t dt  = now - lastEdgeUs;
  if (dt < REV_DEBOUNCE_US) {
    return;                     // bounce, ignore
  }
  revPeriodUs = dt;
  lastEdgeUs  = now;
  revCount++;
}

// L9110S: drive one input with PWM, hold the other low -> constant speed, one way.
void motorRun() {
  analogWrite(pinMotorA, MOTOR_SPEED);
  digitalWrite(pinMotorB, LOW);
}

void motorStop() {
  digitalWrite(pinMotorA, LOW);
  digitalWrite(pinMotorB, LOW);
}

// Latest telemetry decoded from the watch's 7-byte frame.
uint8_t lastHr = 0;                       // bpm
int16_t lastAx = 0, lastAy = 0, lastAz = 0;   // milli-g
bool    haveTelemetry = false;

// Read a big-endian signed 16-bit value from two bytes.
static int16_t readI16BE(const uint8_t* p) {
  return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

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
    Serial.println("master connected");   // watch (BLE central) linked up
  }
  void onDisconnect(BLEServer* s) override {
    deviceConnected = false;
    Serial.println("master disconnected");
    BLEDevice::startAdvertising();        // allow the watch to reconnect
  }
};

class DataCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    uint8_t* data = c->getData();
    size_t   len  = c->getLength();

    // Frame: [HR][N][ N x (accelX,Y,Z int16 BE) ]  -> 2 + N*6 bytes.
    if (len < 2) {
      return;
    }
    uint8_t hr = data[0];
    uint8_t n  = data[1];
    if (len < (size_t)(2 + n * 6)) {
      return;                                  // truncated / malformed
    }

    lastHr = hr;
    targetBrightness = clampByte(hr);          // brightness tracks heart rate

    // Process every accelerometer sample; keep the most recent as "last".
    for (uint8_t i = 0; i < n; i++) {
      const uint8_t* p = &data[2 + i * 6];
      lastAx = readI16BE(p);
      lastAy = readI16BE(p + 2);
      lastAz = readI16BE(p + 4);
    }
    haveTelemetry = true;

    // Throttle serial so high-rate streaming stays readable (~4 lines/sec).
    static uint32_t lastPrint = 0;
    uint32_t now = millis();
    if (now - lastPrint >= 250) {
      lastPrint = now;
      // Tilt from the gravity vector (accel milli-g -> g). Pitch/roll only;
      // yaw is not observable from acceleration alone.
      float gx = lastAx / 1000.0f;
      float gy = lastAy / 1000.0f;
      float gz = lastAz / 1000.0f;
      float pitch = atan2(-gx, sqrt(gy * gy + gz * gz)) * RAD_TO_DEG;
      float roll  = atan2(gy, gz) * RAD_TO_DEG;
      Serial.printf("HR=%3u bpm | accel=(%6d,%6d,%6d) mg | pitch=%6.1f  roll=%6.1f deg\n",
                    lastHr, lastAx, lastAy, lastAz, pitch, roll);
    }
  }
};

void setup() {
  Serial.begin(115200);                   // open Serial Monitor at 115200 baud

  pinMode(pinLow,  INPUT_PULLUP);
  pinMode(pinHigh, INPUT_PULLUP);
  pinMode(ledPin,  OUTPUT);

  pinMode(pinMotorA, OUTPUT);
  pinMode(pinMotorB, OUTPUT);
  pinMode(pinEncoder, INPUT_PULLUP);     // closes to GND -> falling edge per rev
  attachInterrupt(digitalPinToInterrupt(pinEncoder), onRevolution, FALLING);

  targetBrightness = readSwitchTarget();
  lastSwitchTarget = targetBrightness;

  motorRun();                            // spin at constant speed

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

  // Print BLE identity so it can be matched against the Connect IQ app.
  Serial.println("BLE advertising as: " DEVICE_NAME);
  Serial.print("MAC address : ");
  Serial.println(BLEDevice::getAddress().toString().c_str());
  Serial.println("Service UUID: " SERVICE_UUID);
  Serial.println("Data   UUID : " DATA_CHAR_UUID);
  Serial.println("Notify UUID : " NOTIFY_CHAR_UUID);
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

  // Report encoder: total revolutions + RPM from the latest period (~2/sec).
  static uint32_t lastEncPrint = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastEncPrint >= 500) {
    lastEncPrint = nowMs;

    noInterrupts();
    uint32_t period   = revPeriodUs;
    uint32_t count    = revCount;
    uint32_t lastEdge = lastEdgeUs;
    interrupts();

    // Treat the motor as stopped if no pulse for >2 s.
    float rpm = 0.0f;
    if (period > 0 && (micros() - lastEdge) < 2000000UL) {
      rpm = 60000000.0f / (float) period;
    }
    Serial.printf("encoder: revs=%lu  rpm=%.1f\n", (unsigned long) count, rpm);
  }

  delay(5);
}
