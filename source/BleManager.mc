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
    private var _reporter = null;     // periodic sensor telemetry sender

    // Serialized write queue: one outstanding write at a time. The next frame is
    // sent from onCharacteristicWrite, so the BLE stack never holds several writes.
    private const MAX_TX_QUEUE = 24;  // beyond this, drop oldest to favor fresh data
    private var _txQueue = [];
    private var _txBusy = false;

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
        stopReporting();
        resetTx();
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

    // Bluetooth SIG company identifiers -> friendly vendor names.
    private const COMPANY_NAMES = {
        0x004C => "Apple",
        0x0006 => "Microsoft",
        0x0075 => "Samsung",
        0x00E0 => "Google",
        0x0087 => "Garmin",
        0x0059 => "Nordic",
        0x02E5 => "Espressif",
        0x0157 => "Huami",
        0x0499 => "Ruuvi"
    };

    private function addOrUpdate(sr as Ble.ScanResult) {
        // Dedup by real device identity, not by name (many devices are nameless).
        for (var i = 0; i < _discovered.size(); i++) {
            if (_discovered[i][:scan].isSameDevice(sr)) {
                _discovered[i][:rssi]  = sr.getRssi();
                _discovered[i][:scan]  = sr;
                _discovered[i][:label] = deriveLabel(sr);
                return;
            }
        }
        _discovered.add({
            :label => deriveLabel(sr),
            :uuid  => firstUuidString(sr),
            :rssi  => sr.getRssi(),
            :scan  => sr
        });
    }

    private function firstUuidString(sr as Ble.ScanResult) {
        var it = sr.getServiceUuids();
        var u = it.next();
        return (u == null) ? null : u.toString();
    }

    // Best available human label: name -> vendor -> appearance -> unknown.
    private function deriveLabel(sr as Ble.ScanResult) {
        var name = sr.getDeviceName();
        if (name != null && name.length() > 0) {
            return name;
        }
        var vendor = vendorName(sr);
        if (vendor != null) {
            return vendor;
        }
        var appr = appearanceName(sr.getAppearance());
        if (appr != null) {
            return appr;
        }
        return "(unknown)";
    }

    private function vendorName(sr as Ble.ScanResult) {
        var it = sr.getManufacturerSpecificDataIterator();
        if (it == null) {
            return null;
        }
        var entry = it.next();
        if (entry == null) {
            return null;
        }
        var id = entry[:companyId];
        if (id == null) {
            return null;
        }
        if (COMPANY_NAMES.hasKey(id)) {
            return COMPANY_NAMES[id];
        }
        return "Vendor 0x" + id.format("%04X");
    }

    // GAP appearance: top 10 bits are the category.
    private function appearanceName(a) {
        if (a == null || a == 0) {
            return null;
        }
        var cat = a >> 6;
        if (cat == 1)  { return "Phone"; }
        if (cat == 2)  { return "Computer"; }
        if (cat == 3)  { return "Watch"; }
        if (cat == 5)  { return "Display"; }
        if (cat == 8)  { return "Tag"; }
        if (cat == 13) { return "HR Sensor"; }
        if (cat == 18) { return "Cycling Sensor"; }
        return "Appearance " + a;
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
            startReporting();
        } else {
            stopReporting();
            resetTx();
            _device = null;
            setState("disconnected");
        }
    }

    // Start the periodic sensor telemetry sender (idempotent).
    private function startReporting() {
        if (_reporter == null) {
            _reporter = new SensorReporter();
        }
        _reporter.start();
    }

    private function stopReporting() {
        if (_reporter != null) {
            _reporter.stop();
        }
    }

    // Queue a byte array for serialized writing to the ESP32's data characteristic.
    function sendData(bytes) {
        if (!isConnected()) {
            return false;
        }
        if (_txQueue.size() >= MAX_TX_QUEUE) {
            _txQueue = _txQueue.slice(1, null);   // drop oldest (stale) frame
        }
        _txQueue.add(bytes);
        pumpTx();
        return true;
    }

    // Send the head of the queue if no write is in flight.
    private function pumpTx() {
        if (_txBusy || _txQueue.size() == 0) {
            return;
        }
        var ch = getDataChar();
        if (ch == null) {
            _txQueue = [];
            return;
        }
        _txBusy = true;
        try {
            ch.requestWrite(_txQueue[0], { :writeType => Ble.WRITE_TYPE_WITH_RESPONSE });
        } catch (ex) {
            System.println("write failed: " + ex.getErrorMessage());
            _txBusy = false;
            _txQueue = _txQueue.slice(1, null);
        }
    }

    private function resetTx() {
        _txQueue = [];
        _txBusy = false;
    }

    function isConnected() {
        return _device != null;
    }

    function onCharacteristicWrite(characteristic, status) {
        if (_txQueue.size() > 0) {
            _txQueue = _txQueue.slice(1, null);
        }
        _txBusy = false;
        pumpTx();
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
