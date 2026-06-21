using Toybox.BluetoothLowEnergy as Ble;
using Toybox.System;
using Toybox.Lang;
using Toybox.WatchUi;

//
// BleManager - the core of "Option 2".
//
// The watch acts as a BLE *central*:
//   1. scan for the ESP32 (matched by our custom service UUID),
//   2. read RSSI from the scan result (-> rough distance),
//   3. connect (pair) to it,
//   4. enable notifications, and
//   5. write sensor payloads to the ESP32's data characteristic.
//
// The UUIDs below MUST match the ESP32 firmware's GATT server.
//
class BleManager extends Ble.BleDelegate {

    // ---- GATT layout (keep in sync with the ESP32) ----
    public const SERVICE_UUID   = Ble.stringToUuid("c3a1f200-9b0e-4f1a-8a01-0e1d2c3b4a59");
    // watch -> ESP32 (we write sensor data here)
    public const DATA_CHAR_UUID = Ble.stringToUuid("c3a1f201-9b0e-4f1a-8a01-0e1d2c3b4a59");
    // ESP32 -> watch (notifications / acks)
    public const NOTIFY_CHAR_UUID = Ble.stringToUuid("c3a1f202-9b0e-4f1a-8a01-0e1d2c3b4a59");

    private var _view;
    private var _device = null;
    private var _scanning = false;

    // Exposed to the view for display.
    public var rssi = null;            // dBm from last matching scan result
    public var connState = "idle";     // human-readable state string

    function initialize(view) {
        BleDelegate.initialize();
        _view = view;
    }

    // Register our GATT profile, become the BLE delegate, and start scanning.
    function start() {
        registerProfiles();
        Ble.setDelegate(self);
        startScan();
    }

    function shutdown() {
        if (_scanning) {
            Ble.setScanState(Ble.SCAN_STATE_OFF);
            _scanning = false;
        }
        if (_device != null) {
            Ble.unpairDevice(_device);
            _device = null;
        }
    }

    // Describe the service/characteristics we expect so the SDK surfaces them.
    function registerProfiles() {
        var profile = {
            :uuid => SERVICE_UUID,
            :characteristics => [
                { :uuid => DATA_CHAR_UUID },
                {
                    :uuid => NOTIFY_CHAR_UUID,
                    :descriptors => [ Ble.cccdUuid() ]   // 0x2902, to toggle notifications
                }
            ]
        };
        Ble.registerProfile(profile);
    }

    function startScan() {
        _scanning = true;
        setState("scanning");
        Ble.setScanState(Ble.SCAN_STATE_SCANNING);
    }

    function stopScan() {
        _scanning = false;
        Ble.setScanState(Ble.SCAN_STATE_OFF);
    }

    // Write a byte array to the ESP32's data characteristic.
    // Returns true if the write was requested, false if not connected/ready.
    function sendData(bytes) {
        var ch = getDataChar();
        if (ch == null) {
            return false;
        }
        try {
            ch.requestWrite(bytes, { :writeType => Ble.WRITE_TYPE_WITH_RESPONSE });
            return true;
        } catch (ex) {
            System.println("write failed: " + ex.getErrorMessage());
            return false;
        }
    }

    public function isConnected() {
        return _device != null;
    }

    // ---------------- BleDelegate callbacks ----------------

    function onScanResults(scanResults) {
        for (var r = scanResults.next(); r != null; r = scanResults.next()) {
            var sr = r as Ble.ScanResult;
            if (matchesService(sr)) {
                rssi = sr.getRssi();
                setState("connecting");
                stopScan();
                _device = Ble.pairDevice(sr);
                return;
            }
        }
    }

    function onConnectedStateChanged(device, state) {
        if (state == Ble.CONNECTION_STATE_CONNECTED) {
            _device = device;
            setState("connected");
            enableNotifications();
        } else {
            _device = null;
            setState("disconnected");
            startScan();   // try to find it again
        }
    }

    function onCharacteristicWrite(characteristic, status) {
        // status == Ble.STATUS_SUCCESS means the ESP32 acked our write.
    }

    function onCharacteristicChanged(characteristic, value) {
        // A notification from the ESP32 arrived.
        System.println("notify <- " + value);
    }

    function onDescriptorWrite(descriptor, status) {
        // CCCD write result (notifications enabled/disabled).
    }

    function onProfileRegister(uuid, status) {
        System.println("profile register status=" + status);
    }

    // ---------------- helpers ----------------

    private function matchesService(scanResult as Ble.ScanResult) {
        var uuids = scanResult.getServiceUuids();
        for (var u = uuids.next(); u != null; u = uuids.next()) {
            if (u.equals(SERVICE_UUID)) {
                return true;
            }
        }
        return false;
    }

    private function enableNotifications() {
        var ch = getNotifyChar();
        if (ch == null) {
            return;
        }
        var cccd = ch.getDescriptor(Ble.cccdUuid());
        if (cccd != null) {
            cccd.requestWrite([0x01, 0x00]b);   // enable notifications
        }
    }

    private function getService() {
        if (_device == null) {
            return null;
        }
        return _device.getService(SERVICE_UUID);
    }

    private function getDataChar() {
        var svc = getService();
        return (svc == null) ? null : svc.getCharacteristic(DATA_CHAR_UUID);
    }

    private function getNotifyChar() {
        var svc = getService();
        return (svc == null) ? null : svc.getCharacteristic(NOTIFY_CHAR_UUID);
    }

    private function setState(s) {
        connState = s;
        if (_view != null) {
            _view.updateState(connState, rssi);
        }
        WatchUi.requestUpdate();
    }
}
