// Julight - ESP32 light controller with BLE.
//
// Pulses a light (IRLZ44N on GPIO 27) with a sinusoidal "breathing" brightness
// whose frequency is the heart rate: a calm DEFAULT_HR (75 bpm) with no watch
// connected, the watch's live (higher) HR while connected, back to default on
// disconnect. Also positions a motor (L9110S) using a microswitch encoder as the
// 0-degree reference. Exposes a BLE peripheral for a Garmin watch.
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
//
// Motor motion (open-loop, constant speed; encoder pulse = 0 degrees):
//   On boot it spins CALIB_REVS revolutions to learn the time per revolution,
//   plays a light animation, then positions at 180 deg. Serial commands:
//     D<0-360>  move to that absolute position (e.g. "D65")
//     R         recalibrate
//
// WiFi audio mode (overrides the HR pulse while packets arrive):
//   Joins a WiFi network and listens for UDP brightness frames on AUDIO_UDP_PORT.
//   Each datagram's first byte (0..255) sets the light directly (still scaled by
//   the toggle switch). A companion Python script analyses an MP3, runs an FFT,
//   maps the bass energy to brightness and streams these bytes ~60x/sec. After
//   AUDIO_TIMEOUT_MS with no packet, the light reverts to the HR breathing pulse.

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#define SERVICE_UUID     "c3a1f200-9b0e-4f1a-8a01-0e1d2c3b4a59"
#define DATA_CHAR_UUID   "c3a1f201-9b0e-4f1a-8a01-0e1d2c3b4a59"
#define NOTIFY_CHAR_UUID "c3a1f202-9b0e-4f1a-8a01-0e1d2c3b4a59"
#define DEVICE_NAME      "Julight-ESP32"

// --- WiFi audio link -------------------------------------------------------
// Fill in your network. The ESP32 joins as a station; the Python script sends
// UDP brightness frames to this device's IP on AUDIO_UDP_PORT (printed at boot).
#define WIFI_SSID      "4-04"
#define WIFI_PASSWORD  "Viernulvier"
const uint16_t AUDIO_UDP_PORT   = 4210;   // UDP port the brightness stream targets
const uint32_t AUDIO_TIMEOUT_MS = 300;    // no packet for this long -> back to HR pulse

WiFiUDP audioUdp;
volatile int      audioBrightness = 0;    // last brightness byte received over UDP
volatile uint32_t lastAudioMs     = 0;    // millis() of the last UDP frame (0 = none)

// Audio frames arrive ~60x/sec and jump abruptly, which reads as flicker when
// written raw. Low-pass the level: each loop the shown brightness eases a
// fraction (AUDIO_SMOOTH) of the way toward the latest target. Smaller = smoother
// but laggier; ~0.18 at the 5 ms loop gives a ~25 ms time constant.
const float AUDIO_SMOOTH    = 0.18f;
float       smoothedAudio   = 0.0f;       // filtered audio brightness (0..255)

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

// Drive PWM (0..255) used for BOTH calibration and positioning. They must match:
// the move time is derived from the calibrated revolution time, so positioning is
// only accurate if the motor runs at the same speed it was calibrated at.
const int MOTOR_SPEED = 200;

// Motor coast after motorStop(): the move time is shortened by this to land on
// target instead of overshooting. Tune to 0 first, then raise if it overshoots.
const uint32_t STOP_LAG_US = 0;

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
// The center is kept low (dim baseline) and the swing wide, so the pulse is
// clearly visible yet never near full brightness (255 reserved for feedback).
const float PULSE_CENTER  = 70.0f;     // mid brightness the pulse swings around
const float PULSE_AMP_MIN = 45.0f;     // swing at PULSE_HR_MIN (clearly visible)
const float PULSE_AMP_MAX = 70.0f;     // swing at PULSE_HR_MAX (deep, dramatic)
const float PULSE_HR_MIN  = 75.0f;     // HR mapped to AMP_MIN
const float PULSE_HR_MAX  = 180.0f;    // HR mapped to AMP_MAX

// Encoder state (polled with debounce; no interrupt).
uint32_t revCount    = 0;            // total revolutions counted
uint32_t revPeriodUs = 0;            // micros between the last two revolutions
uint32_t lastRevUs   = 0;            // edge time of the last counted closure
int      swStable    = HIGH;         // debounced contact state (HIGH = open)
int      swLastRaw   = HIGH;         // last raw reading, for settle timing
uint32_t swChangeMs  = 0;            // when the raw reading last changed
uint32_t candidateEdgeUs = 0;        // time of the pending HIGH->LOW edge

// --- Motion controller -----------------------------------------------------
// Open-loop: the motor only runs (at MOTOR_SPEED) to calibrate or to move, then
// stops and holds. The encoder pulse marks 0 deg. Calibration learns the time
// for one revolution; to reach an angle we wait for a 0-pulse then run
// (angle/360) of that time and stop. Accuracy relies on the move speed matching
// the calibration speed, so both use MOTOR_SPEED.
const uint32_t CALIB_REVS    = 4;       // revolutions sampled during calibration
const uint32_t CALIB_ANIM_MS = 1500;    // "calibration done" light animation

enum Mode { CALIBRATING, CALIB_ANIM, POSITIONING, HOLDING };
Mode mode = CALIBRATING;

uint32_t revolutionPeriodUs = 0;        // learned during calibration (0 = not yet)
float    targetDeg          = 180.0f;   // position to seek after calibration

bool     calibArmed     = false;        // first 0-pulse seen, clock started
uint32_t calibStartUs   = 0;
uint32_t calibStartRev  = 0;
uint32_t calibAnimStart = 0;

bool     posSynced = false;             // reference 0-pulse seen for this move
uint32_t posRefRev = 0;                 // revCount value of that reference pulse
uint32_t posStopUs = 0;                 // when to stop the motor

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
    if (raw == LOW) {                // remember the actual contact instant...
      candidateEdgeUs = micros();    // ...so timing isn't delayed by debounce
    }
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
    revPeriodUs = candidateEdgeUs - lastRevUs;
    lastRevUs   = candidateEdgeUs;   // use the true edge time, not settle time
    revCount++;
  }
}

// L9110S: PWM one input, the other at 0 -> drive one direction. Use analogWrite
// on BOTH pins (never digitalWrite) so duty 0 truly releases the LEDC channel --
// mixing digitalWrite after analogWrite can leave the PWM running on ESP32.
void motorRun() {
  analogWrite(pinMotorA, MOTOR_SPEED);
  analogWrite(pinMotorB, 0);
  Serial.printf("motor: ENABLED (PWM %d)\n", MOTOR_SPEED);
}

// Disable the driver: both inputs at 0 -> motor de-energized (coasts free). Stays
// off until the next move (D) or recalibration (R) calls motorRun() again.
void motorStop() {
  analogWrite(pinMotorA, 0);
  analogWrite(pinMotorB, 0);
  Serial.println("motor: DISABLED");
}

// Start (or restart) calibration: spin and learn the revolution time.
void startCalibration() {
  mode = CALIBRATING;
  calibArmed = false;
  motorRun();
  Serial.println("calibrating: measuring revolution time...");
}

// Begin moving to targetDeg by timing forward from the first 0-pulse after start.
// The travel from the held position to the 0 mark gives the motor time to reach
// speed, so no extra warm-up lap is needed.
void startPositioning() {
  if (revolutionPeriodUs == 0) {
    Serial.println("not calibrated yet - send R first");
    return;
  }
  mode = POSITIONING;
  posSynced = false;
  posRefRev = revCount + 1;            // reference the next 0-pulse
  motorRun();
  Serial.printf("positioning to %d deg...\n", (int) targetDeg);
}

void updateCalibration() {
  if (!calibArmed) {
    if (revCount >= 1) {               // first 0-pulse: start the clock
      calibArmed    = true;
      calibStartUs  = lastRevUs;
      calibStartRev = revCount;
    }
    return;
  }
  uint32_t revs = revCount - calibStartRev;
  if (revs >= CALIB_REVS) {
    revolutionPeriodUs = (lastRevUs - calibStartUs) / revs;
    Serial.printf("calibrated: %lu rev -> period %lu ms\n",
                  (unsigned long) revs,
                  (unsigned long) (revolutionPeriodUs / 1000));
    motorStop();
    mode = CALIB_ANIM;
    calibAnimStart = millis();
  }
}

void updatePositioning() {
  if (!posSynced) {
    if (revCount >= posRefRev) {        // reference 0-pulse reached
      posSynced = true;
      uint32_t runUs = (uint32_t)((targetDeg / 360.0f) * (float) revolutionPeriodUs);
      runUs = (runUs > STOP_LAG_US) ? (runUs - STOP_LAG_US) : 0;   // coast comp.
      posStopUs = lastRevUs + runUs;
    }
    return;
  }
  if ((int32_t)(micros() - posStopUs) >= 0) {
    motorStop();
    mode = HOLDING;
    Serial.printf("positioned at %d deg\n", (int) targetDeg);
  }
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

// Sinusoidal "breathing" pulse at the active heart rate (toggle switch scales).
void updateHrLight(float scale) {
  static uint32_t lastUs = micros();
  uint32_t nowUs = micros();
  float dt = (nowUs - lastUs) / 1000000.0f;     // seconds since last call
  lastUs = nowUs;

  // Ease the effective HR so frequency and amplitude shift gently, not abruptly.
  static float smoothHr = DEFAULT_HR;
  smoothHr += (activeHrBpm - smoothHr) * 0.02f;

  float freq = smoothHr / 60.0f;                // beats per second
  pulsePhase += 2.0f * PI * freq * dt;
  if (pulsePhase >= 2.0f * PI) {
    pulsePhase -= 2.0f * PI;
  }

  float hrSpan = (smoothHr - PULSE_HR_MIN) / (PULSE_HR_MAX - PULSE_HR_MIN);
  if (hrSpan < 0.0f) { hrSpan = 0.0f; }
  if (hrSpan > 1.0f) { hrSpan = 1.0f; }
  float amp = PULSE_AMP_MIN + (PULSE_AMP_MAX - PULSE_AMP_MIN) * hrSpan;

  float level = PULSE_CENTER + amp * sinf(pulsePhase);
  int b = (int)(level * scale + 0.5f);
  if (b < 0)   { b = 0; }
  if (b > 255) { b = 255; }
  currentBrightness = b;
  analogWrite(ledPin, currentBrightness);
}

// Connect to WiFi (non-fatal: if it fails, BLE/HR still work) and start the UDP
// listener. Tries for ~10 s so a slow AP doesn't hang boot forever.
void connectWiFi() {
  Serial.printf("WiFi: connecting to \"%s\" ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    audioUdp.begin(AUDIO_UDP_PORT);
    Serial.print("WiFi: connected, IP = ");
    Serial.println(WiFi.localIP());
    Serial.printf("Audio: streaming brightness bytes to UDP %s:%u\n",
                  WiFi.localIP().toString().c_str(), AUDIO_UDP_PORT);
  } else {
    Serial.println("WiFi: not connected (audio mode disabled; HR pulse still runs)");
  }
}

// Drain any pending UDP brightness frames; the last byte of the newest packet
// wins. Stamps lastAudioMs so updateLight() knows the audio stream is live.
void pollAudioUdp() {
  static uint8_t pkt[64];
  int sz;
  while ((sz = audioUdp.parsePacket()) > 0) {
    int n = audioUdp.read(pkt, sizeof(pkt));
    if (n > 0) {
      audioBrightness = pkt[0];           // byte 0 = brightness 0..255
      lastAudioMs = millis();
    }
  }
}

// Pick the light source: the audio stream overrides while it is live, otherwise
// fall back to the HR breathing pulse. The toggle switch still scales both.
void updateLight(float scale) {
  if (lastAudioMs != 0 && (millis() - lastAudioMs) < AUDIO_TIMEOUT_MS) {
    // Ease the filtered level toward the latest target, then scale + clamp.
    smoothedAudio += (audioBrightness - smoothedAudio) * AUDIO_SMOOTH;
    int b = (int)(smoothedAudio * scale + 0.5f);
    if (b < 0)   { b = 0; }
    if (b > 255) { b = 255; }
    currentBrightness = b;
    analogWrite(ledPin, currentBrightness);
  } else {
    updateHrLight(scale);
  }
}

// "Calibration done" highlight: quick bright flashes, then go position to target.
void updateCalibAnim(float scale) {
  uint32_t t = millis() - calibAnimStart;
  if (t >= CALIB_ANIM_MS) {
    startPositioning();                          // seek targetDeg (180 by default)
    return;
  }
  bool on = ((t / 150) % 2) == 0;               // ~3 flashes per second
  currentBrightness = (int)((on ? 255.0f : 0.0f) * scale + 0.5f);
  analogWrite(ledPin, currentBrightness);
}

// Print one line per completed revolution.
void reportEncoder() {
  static uint32_t lastReportedCount = 0;
  if (revCount == lastReportedCount) {
    return;
  }
  lastReportedCount = revCount;
  if (revCount >= 2) {
    float rpm = 60000000.0f / (float) revPeriodUs;
    Serial.printf("rev #%lu  period=%lu ms  rpm=%.2f\n",
                  (unsigned long) revCount,
                  (unsigned long) (revPeriodUs / 1000), rpm);
  } else {
    Serial.printf("rev #%lu  (timing starts next revolution)\n",
                  (unsigned long) revCount);
  }
}

// Parse a serial command line: "D<0-360>" to position, "R" to recalibrate.
void processCommand(const char* s) {
  if (s[0] == 'R' || s[0] == 'r') {
    startCalibration();
    return;
  }
  if (s[0] == 'D' || s[0] == 'd') {
    int deg = atoi(s + 1);
    if (deg < 0)   { deg = 0; }
    if (deg > 360) { deg = 360; }
    targetDeg = (float) deg;
    startPositioning();
    return;
  }
  Serial.printf("unknown cmd '%s' (use D<0-360> or R)\n", s);
}

void handleSerial() {
  static char buf[16];
  static uint8_t len = 0;
  while (Serial.available()) {
    char ch = (char) Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (len > 0) {
        buf[len] = '\0';
        processCommand(buf);
        len = 0;
      }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = ch;
    }
  }
}

void setup() {
  Serial.begin(115200);                   // open Serial Monitor at 115200 baud

  pinMode(pinLow,  INPUT_PULLUP);
  pinMode(pinHigh, INPUT_PULLUP);
  pinMode(ledPin,  OUTPUT);

  pinMode(pinMotorA, OUTPUT);
  pinMode(pinMotorB, OUTPUT);
  pinMode(pinEncoder, INPUT_PULLUP);     // closes to GND; polled in pollEncoder()

  startCalibration();                    // spin up and learn the revolution time

  connectWiFi();                         // join network + open UDP audio listener

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
  Serial.println("Motor cmds  : D<0-360> to position, R to recalibrate");
}

void loop() {
  float scale = readSwitchTarget() / 255.0f;   // toggle switch brightness scale

  handleSerial();                              // D<0-360> / R commands
  pollEncoder();                               // keep revCount current
  pollAudioUdp();                              // ingest WiFi brightness frames

  switch (mode) {
    case CALIBRATING:
      updateCalibration();
      updateLight(scale);                      // audio overrides, else HR pulse
      break;
    case CALIB_ANIM:
      updateCalibAnim(scale);                  // flashes, then starts positioning
      break;
    case POSITIONING:
      updatePositioning();
      updateLight(scale);
      break;
    case HOLDING:
      updateLight(scale);
      break;
  }

  reportEncoder();
  delay(5);
}
