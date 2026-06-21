using Toybox.Sensor;
using Toybox.Timer;
using Toybox.System;
using Toybox.Lang;

//
// SensorReporter - periodically pushes heart rate + accelerometer to the ESP32.
//
// Runs only while the BLE link is up (started/stopped from BleManager). Every
// PERIOD_MS it reads the latest sensor sample and writes a fixed 7-byte frame
// to the ESP32's data characteristic via gBle.sendData().
//
// Payload (big-endian):
//   byte 0    : heart rate, bpm           (0..255, 0 = no reading)
//   bytes 1-2 : accel X, milli-g          (int16, signed)
//   bytes 3-4 : accel Y, milli-g          (int16, signed)
//   bytes 5-6 : accel Z, milli-g          (int16, signed)
//
// Note: the current ESP32 firmware reads byte 0 as brightness, so as-is the
// light will track heart rate. Reorder the frame if that's not desired.
//
class SensorReporter {

    private const PERIOD_MS = 2000;     // send every 2 seconds

    private var _timer = null;

    // Begin sampling sensors and start the periodic send timer.
    function start() {
        if (_timer != null) {
            return;                     // already running
        }
        Sensor.setEnabledSensors([Sensor.SENSOR_HEARTRATE]);
        Sensor.enableSensorEvents(method(:onSensor));

        _timer = new Timer.Timer();
        _timer.start(method(:onTick), PERIOD_MS, true);   // repeating
    }

    // Stop the timer and release the sensors.
    function stop() {
        if (_timer != null) {
            _timer.stop();
            _timer = null;
        }
        Sensor.enableSensorEvents(null);
    }

    // Sensor.Info arrives here; we just read fresh data in onTick() instead.
    function onSensor(info as Sensor.Info) as Void {
    }

    // Build the telemetry frame and send it if still connected.
    function onTick() as Void {
        if (gBle == null || !gBle.isConnected()) {
            return;
        }

        var info = Sensor.getInfo();
        var hr = 0;
        var ax = 0;
        var ay = 0;
        var az = 0;

        if (info != null) {
            if (info.heartRate != null) {
                hr = info.heartRate;
            }
            if (info.accel != null) {
                ax = info.accel[0];
                ay = info.accel[1];
                az = info.accel[2];
            }
        }

        var frame = []b;
        frame.add(clampByte(hr));
        appendI16(frame, ax);
        appendI16(frame, ay);
        appendI16(frame, az);

        var ok = gBle.sendData(frame);
        System.println("telemetry -> hr=" + hr +
                       " accel=[" + ax + "," + ay + "," + az + "] sent=" + ok);
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
