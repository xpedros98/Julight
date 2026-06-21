using Toybox.WatchUi;
using Toybox.Graphics;
using Toybox.Math;

// Shown after a device is selected: connection state, RSSI/distance,
// and a hint for sending data. The ESP32 receives writes here.
class StatusView extends WatchUi.View {

    private var _state = "connecting";
    private var _rssi = null;
    private var _sent = 0;

    function initialize() {
        View.initialize();
    }

    function onShow() {
        if (gBle != null) {
            gBle.setStatusView(self);
            _state = gBle.connState;
            _rssi = gBle.rssi;
            _sent = gBle.txCount;
        }
    }

    function onHide() {
        if (gBle != null) {
            gBle.setStatusView(null);
        }
    }

    // Called by BleManager when state / RSSI changes.
    function updateState(state, rssi) {
        _state = state;
        _rssi = rssi;
    }

    // Called by BleManager on each confirmed write (telemetry feedback).
    function onSent(count) {
        _sent = count;
    }

    function onUpdate(dc) {
        dc.setColor(Graphics.COLOR_WHITE, Graphics.COLOR_BLACK);
        dc.clear();

        var cx = dc.getWidth() / 2;
        var cy = dc.getHeight() / 2;

        dc.drawText(cx, cy - 60, Graphics.FONT_SMALL, "Julight BLE",
            Graphics.TEXT_JUSTIFY_CENTER);
        dc.drawText(cx, cy - 32, Graphics.FONT_MEDIUM, _state,
            Graphics.TEXT_JUSTIFY_CENTER);

        var rssiText = (_rssi == null) ? "RSSI: --" : ("RSSI: " + _rssi + " dBm");
        dc.drawText(cx, cy + 8, Graphics.FONT_SMALL, rssiText,
            Graphics.TEXT_JUSTIFY_CENTER);

        if (_rssi != null) {
            dc.drawText(cx, cy + 32, Graphics.FONT_SMALL,
                "~ " + estimateDistance(_rssi) + " m",
                Graphics.TEXT_JUSTIFY_CENTER);
        }

        dc.drawText(cx, cy + 60, Graphics.FONT_SMALL, "sent: " + _sent,
            Graphics.TEXT_JUSTIFY_CENTER);

        dc.drawText(cx, dc.getHeight() - 26, Graphics.FONT_XTINY,
            "START: send", Graphics.TEXT_JUSTIFY_CENTER);
    }

    // Crude log-distance estimate (txPower at 1 m = -59 dBm, n = 2.0).
    private function estimateDistance(rssi) {
        var txPower = -59.0;
        var n = 2.0;
        var d = Math.pow(10, (txPower - rssi) / (10.0 * n));
        return Math.round(d * 10) / 10.0;
    }
}
