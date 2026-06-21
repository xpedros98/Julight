// Julight - ESP32 light controller with BLE.
//
// Pulses a light (IRLZ44N on GPIO 27) with a sinusoidal "breathing" brightness
// whose frequency is the heart rate: a calm DEFAULT_HR (75 bpm) with no watch
// connected, the watch's live (higher) HR while connected, back to default on
// disconnect. Also drives a constant-speed motor (L9110S) and counts revolutions
// from a microswitch encoder. Exposes a BLE peripheral for a Garmin watch.
//
// BLE contract (must match the Connect IQ app):
//   Service : c3a1f200-9b0e-4f1a-8a01-0e1d2c3b4a59  (advertised)
//   Data    : c3a1f201-...  watch -> ESP32, WRITE   (telemetry frame, see below)
//   Notify  : c3a1f202-...  ESP32 -> watch, NOTIFY  (unused)
//
//   Data frame: [HR][N][ N x (accelX,Y,Z int16 big-endian, milli-g) ].
//   Byte 0 (HR) sets the pulse frequency.
//
// The toggle switch (pins 4/5) scales overall brightness: OFF / MED / FULL.

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

// Encoder contact must be stable (settled) this long before a make/break is
// accepted. Filters bounce/chatter; keep it shorter than the switch's closed
// time so a real closure is not missed.
const uint32_t SW_DEBOUNCE_MS = 40;

const int DEFAULT_HR = 75;             // pulse rate (bpm) with no watch connected
volatile int activeHrBpm = DEFAULT_HR; // current HR driving the light pulse
int   currentBrightness  = 0;          // last value written to the light
float pulsePhase         = 0.0f;       // sinusoid phase, radians

// Light pulse shaping: brightness = CENTER +/- amplitude*sin(phase), where the
// amplitude grows with HR -> a subtle glow at rest, a wider swing when racing.
const float PULSE_CENTER  = 128.0f;    // mid brightness the pulse swings around
const float PULSE_AMP_MIN = 18.0f;     // swing at PULSE_HR_MIN (gentle)
const float PULSE_AMP_MAX = 100.0f;    // swing at PULSE_HR_MAX (dramatic)
const float PULSE_HR_MIN  = 75.0f;     // HR mapped to AMP_MIN
const float PULSE_HR_MAX  = 180.0f;    // HR mapped to AMP_MAX

// Encoder state (polled with debounce; no interrupt).
uint32_t revCount    = 0;            // total revolutions counted
uint32_t revPeriodUs = 0;            // micros between the last two revolutions
uint32_t lastRevUs   = 0;            // timestamp of the last counted closure
int      swStable    = HIGH;         // debounced contact state (HIGH = open)
int      swLastRaw   = HIGH;         // last raw reading, for settle timing
uint32_t swChangeMs  = 0;            // when the raw reading last changed

bool deviceConnected = false;

BLECharacteristic* notifyChar = nullptr;

// Poll the microswitch with a require-release debounce: count one revolution per
// clean closure (HIGH->LOW) only after the contact has settled, and don't count
// again until it has cleanly opened. This rejects bounce and the long, chattery
// make/break of a slow cam that an edge interrupt double-counts.
void pollEncoder() {
  int raw = digitalRead(pinEncoder);
  uint32_t nowMs = millis();

  if (raw != swLastRaw) {            // raw moved: restart the settle timer
    swLastRaw  = raw;
    swChangeMs = nowMs;
    return;
  }
  if (nowMs - swChangeMs < SW_DEBOUNCE_MS) {
    return;                          // not settled yet
  }
  if (raw == swStable) {
    return;                          // settled, but no new state
  }

  swStable = raw;                    // accept the debounced transition
  if (swStable == LOW) {             // confirmed closure = one revolution
    uint32_t nowUs = micros();
    revPeriodUs = nowUs - lastRevUs;
    lastRevUs   = nowUs;
    revCount++;
  }
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
    activeHrBpm = DEFAULT_HR;             // fall back to the default pulse rate
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
    activeHrBpm = (hr > 0) ? hr : DEFAULT_HR;   // HR sets the pulse frequency

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
  pinMode(pinEncoder, INPUT_PULLUP);     // closes to GND; polled in pollEncoder()

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
  // Sinusoidal "breathing" pulse at the active heart rate. Advance the phase by
  // the elapsed time so changing HR shifts frequency without any discontinuity.
  // The toggle switch scales overall brightness (OFF=0, MED~half, FULL=full).
  float scale = readSwitchTarget() / 255.0f;

  static uint32_t lastUs = micros();
  uint32_t nowUs = micros();
  float dt = (nowUs - lastUs) / 1000000.0f;     // seconds since last loop
  lastUs = nowUs;

  // Ease the effective HR toward the target so frequency and amplitude shift
  // gently instead of jumping when a new reading arrives.
  static float smoothHr = DEFAULT_HR;
  smoothHr += (activeHrBpm - smoothHr) * 0.02f;

  // Advance phase at the heart-rate frequency (continuous across HR changes).
  float freq = smoothHr / 60.0f;                // beats per second
  pulsePhase += 2.0f * PI * freq * dt;
  if (pulsePhase >= 2.0f * PI) {
    pulsePhase -= 2.0f * PI;
  }

  // Amplitude grows with HR: gentle glow at rest, wider swing when racing.
  float hrSpan = (smoothHr - PULSE_HR_MIN) / (PULSE_HR_MAX - PULSE_HR_MIN);
  if (hrSpan < 0.0f) { hrSpan = 0.0f; }
  if (hrSpan > 1.0f) { hrSpan = 1.0f; }
  float amp = PULSE_AMP_MIN + (PULSE_AMP_MAX - PULSE_AMP_MIN) * hrSpan;

  float level = PULSE_CENTER + amp * sinf(pulsePhase);   // before toggle scaling
  int b = (int)(level * scale + 0.5f);
  if (b < 0)   { b = 0; }
  if (b > 255) { b = 255; }
  currentBrightness = b;
  analogWrite(ledPin, currentBrightness);

  // Poll the encoder switch (debounced) and report once per completed revolution.
  pollEncoder();
  static uint32_t lastReportedCount = 0;
  uint32_t count  = revCount;
  uint32_t period = revPeriodUs;

  if (count != lastReportedCount) {
    lastReportedCount = count;
    if (count >= 2) {                        // first pulse has no prior edge to time from
      float rpm = 60000000.0f / (float) period;
      Serial.printf("rev #%lu  period=%lu ms  rpm=%.2f\n",
                    (unsigned long) count, (unsigned long)(period / 1000), rpm);
    } else {
      Serial.printf("rev #%lu  (timing starts next revolution)\n",
                    (unsigned long) count);
    }
  }

  delay(5);
}
