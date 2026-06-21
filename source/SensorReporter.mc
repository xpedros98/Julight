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
// Each callback's samples are chunked into BLE frames and queued for serialized
// writing (BleManager drains the queue one write at a time, so nothing is lost).
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
        var options = {
            :period => 1,                       // one callback per second
            :accelerometer => {
                :enabled    => true,
                :sampleRate => SAMPLE_RATE
            },
            :heartRate => {
                :enabled => true
            }
        };
        Sensor.registerSensorDataListener(method(:onData), options);
        _listening = true;
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
        var i = 0;
        while (i < n) {
            var count = n - i;
            if (count > SAMPLES_PER_FRAME) {
                count = SAMPLES_PER_FRAME;
            }

            var frame = []b;
            frame.add(clampByte(hr));
            frame.add(count);
            for (var j = 0; j < count; j++) {
                appendI16(frame, xs[i + j]);
                appendI16(frame, ys[i + j]);
                appendI16(frame, zs[i + j]);
            }
            gBle.sendData(frame);
            i += count;
        }
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
