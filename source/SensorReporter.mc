using Toybox.Sensor;
using Toybox.System;
using Toybox.Lang;

//
// SensorReporter - streams heart rate + high-rate accelerometer to the ESP32.
//
// Uses Sensor.registerSensorDataListener so the OS batches accelerometer samples
// (SAMPLE_RATE Hz) and delivers them once per :period second. That is far cheaper
// than waking a Timer and polling Sensor.getInfo(), and gives real motion data.
//
// Each callback sends ONE compact frame (a few accel samples + HR) so the BLE
// link is never flooded; BleManager serializes writes and counts each one.
//
// Wire frame (big-endian), variable length:
//   byte 0      : heart rate, bpm                 (0..255, 0 = no reading)
//   byte 1      : N = accel sample count in frame (1..SAMPLES_PER_FRAME)
//   then N times: accel X,Y,Z as int16 (milli-g)  (6 bytes each)
//
// SAMPLES_PER_FRAME is sized so one frame fits a conservative 20-byte BLE write
// (2 + 3*6 = 20). Orientation (pitch/roll) is computed on the ESP32.
//
class SensorReporter {

    private const SAMPLE_RATE       = 25;   // accel samples per second
    private const SAMPLES_PER_FRAME = 3;    // keeps each frame within 20 bytes

    private var _listening = false;

    // Begin batched sensor sampling.
    function start() as Void {
        if (_listening) {
            return;
        }
        // NOTE: valid option keys are :accelerometer / :gyroscope / :magnetometer
        // / :heartBeatIntervals. There is no :heartRate key (an invalid key makes
        // the accelerometer config fail and return zeros). HR comes from getInfo().
        var options = {
            :period => 1,                       // one callback per second
            :accelerometer => {
                :enabled    => true,
                :sampleRate => SAMPLE_RATE
            }
        };
        try {
            Sensor.registerSensorDataListener(method(:onData), options);
            _listening = true;
        } catch (ex) {
            System.println("registerSensorDataListener failed: " + ex.getErrorMessage());
        }
    }

    // Stop the listener and release the sensors.
    function stop() as Void {
        if (!_listening) {
            return;
        }
        Sensor.unregisterSensorDataListener();
        _listening = false;
    }

    // A batch of samples for the elapsed period arrives here.
    function onData(data as Sensor.SensorData) as Void {
        if (gBle == null || !gBle.isConnected()) {
            return;
        }

        var hr = latestHeartRate();

        var accel = data.accelerometerData;
        if (accel == null) {
            return;
        }
        var xs = accel.x;
        var ys = accel.y;
        var zs = accel.z;
        if (xs == null || ys == null || zs == null) {
            return;
        }

        var n = xs.size();
        if (n == 0) {
            return;
        }

        // Send exactly ONE frame per callback (~1/s): up to SAMPLES_PER_FRAME
        // accel samples spread across the batch. Sending every 25 Hz sample
        // floods the BLE link (writes can't drain that fast) and builds a
        // backlog; one small frame keeps latency low and feedback meaningful.
        var count = (n < SAMPLES_PER_FRAME) ? n : SAMPLES_PER_FRAME;
        var frame = []b;
        frame.add(clampByte(hr));
        frame.add(count);
        for (var k = 0; k < count; k++) {
            var idx = (count == 1) ? (n - 1) : (k * (n - 1) / (count - 1));
            appendI16(frame, xs[idx]);
            appendI16(frame, ys[idx]);
            appendI16(frame, zs[idx]);
        }
        gBle.sendData(frame);
    }

    // Current heart rate in bpm (cached, ~1 Hz), or 0 if none. The data listener
    // only reports beat-to-beat intervals, so we read bpm from getInfo() instead.
    private function latestHeartRate() {
        var info = Sensor.getInfo();
        if (info != null && info.heartRate != null) {
            return info.heartRate;
        }
        return 0;
    }

    private function clampByte(v) {
        if (v < 0)   { return 0; }
        if (v > 255) { return 255; }
        return v;
    }

    // Append a signed value as a big-endian int16 (two bytes).
    private function appendI16(buf, v) {
        var iv = v.toNumber();
        buf.add((iv >> 8) & 0xFF);
        buf.add(iv & 0xFF);
    }
}
