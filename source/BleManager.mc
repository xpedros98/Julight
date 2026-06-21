using Toybox.BluetoothLowEnergy as Ble;
using Toybox.System;
using Toybox.Lang;
using Toybox.WatchUi;

//
// BleManager - the watch acts as a BLE *central*.
//
// Flow:
//   beginScan()  -> collect nearby devices into _discovered (name / uuid / rssi)
//   connectTo(scanResult) -> pair, then write sensor payloads over GATT
//
// The data/notify UUIDs must match the ESP32 firmware. The service UUID is what
// the ESP32 advertises; only devices exposing it can actually receive our data,
// but we list every nearby device so the user can choose.
//
class BleManager extends Ble.BleDelegate {

    public const SERVICE_UUID     = Ble.stringToUuid("c3a1f200-9b0e-4f1a-8a01-0e1d2c3b4a59");
    public const DATA_CHAR_UUID   = Ble.stringToUuid("c3a1f201-9b0e-4f1a-8a01-0e1d2c3b4a59");
    public const NOTIFY_CHAR_UUID = Ble.stringToUuid("c3a1f202-9b0e-4f1a-8a01-0e1d2c3b4a59");

    private var _device = null;
    private var _scanning = false;
    private var _discovered = [];     // [{ :key, :name, :uuid, :rssi, :scan }]
    private var _statusView = null;

    public var connState = "idle";
    public var rssi = null;

    function initialize() {
        BleDelegate.initialize();
    }

    // Register our GATT profile and become the BLE delegate (no scan yet).
    function start() {
        registerProfiles();
        Ble.setDelegate(self);
    }

    function shutdown() {
        stopScan();
        if (_device != null) {
            Ble.unpairDevice(_device);
            _device = null;
        }
    }

    function registerProfiles() {
        var profile = {
            :uuid => SERVICE_UUID,
            :characteristics => [
                { :uuid => DATA_CHAR_UUID },
                { :uuid => NOTIFY_CHAR_UUID, :descriptors => [ Ble.cccdUuid() ] }
            ]
        };
        Ble.registerProfile(profile);
    }

    // ---------------- discovery ----------------

    function beginScan() {
        _discovered = [];
        _scanning = true;
        connState = "scanning";
        Ble.setScanState(Ble.SCAN_STATE_SCANNING);
    }

    function stopScan() {
        if (_scanning) {
            _scanning = false;
            Ble.setScanState(Ble.SCAN_STATE_OFF);
        }
    }

    function getDiscovered() {
        return _discovered;
    }

    function onScanResults(scanResults) {
        for (var r = scanResults.next(); r != null; r = scanResults.next()) {
            addOrUpdate(r as Ble.ScanResult);
        }
    }

    private function addOrUpdate(sr as Ble.ScanResult) {
        var name = sr.getDeviceName();
        var uuidStr = firstUuidString(sr);
        var rssiVal = sr.getRssi();
        var key = (name != null) ? name : ((uuidStr != null) ? uuidStr : "(unknown)");

        for (var i = 0; i < _discovered.size(); i++) {
            if (_discovered[i][:key].equals(key)) {
                _discovered[i][:rssi] = rssiVal;
                _discovered[i][:scan] = sr;
                return;
            }
        }
        _discovered.add({
            :key => key, :name => name, :uuid => uuidStr,
            :rssi => rssiVal, :scan => sr
        });
    }

    private function firstUuidString(sr as Ble.ScanResult) {
        var it = sr.getServiceUuids();
        var u = it.next();
        return (u == null) ? null : u.toString();
    }

    // ---------------- connection ----------------

    function connectTo(scanResult as Ble.ScanResult) {
        stopScan();
        rssi = scanResult.getRssi();
        _device = Ble.pairDevice(scanResult);
        setState("connecting");
    }

    function onConnectedStateChanged(device, state) {
        if (state == Ble.CONNECTION_STATE_CONNECTED) {
            _device = device;
            setState("connected");
            enableNotifications();
        } else {
            _device = null;
            setState("disconnected");
        }
    }

    // Write a byte array to the ESP32's data characteristic.
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

    function isConnected() {
        return _device != null;
    }

    function onCharacteristicWrite(characteristic, status) {
    }

    function onCharacteristicChanged(characteristic, value) {
        System.println("notify <- " + value);
    }

    function onDescriptorWrite(descriptor, status) {
    }

    function onProfileRegister(uuid, status) {
        System.println("profile register status=" + status);
    }

    // ---------------- helpers ----------------

    private function enableNotifications() {
        var ch = getNotifyChar();
        if (ch == null) {
            return;
        }
        var cccd = ch.getDescriptor(Ble.cccdUuid());
        if (cccd != null) {
            cccd.requestWrite([0x01, 0x00]b);
        }
    }

    private function getService() {
        return (_device == null) ? null : _device.getService(SERVICE_UUID);
    }

    private function getDataChar() {
        var svc = getService();
        return (svc == null) ? null : svc.getCharacteristic(DATA_CHAR_UUID);
    }

    private function getNotifyChar() {
        var svc = getService();
        return (svc == null) ? null : svc.getCharacteristic(NOTIFY_CHAR_UUID);
    }

    function setStatusView(view) {
        _statusView = view;
    }

    private function setState(s) {
        connState = s;
        if (_statusView != null) {
            _statusView.updateState(connState, rssi);
        }
        WatchUi.requestUpdate();
    }
}
