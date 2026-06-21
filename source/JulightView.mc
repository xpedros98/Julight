using Toybox.WatchUi;
using Toybox.Graphics;
using Toybox.Math;
using Toybox.Lang;

class JulightView extends WatchUi.View {

    private var _state = "starting";
    private var _rssi = null;

    function initialize() {
        View.initialize();
    }

    // Called by BleManager whenever connection state / RSSI changes.
    function updateState(state, rssi) {
        _state = state;
        _rssi = rssi;
    }

    function onLayout(dc) {
    }

    function onUpdate(dc) {
        dc.setColor(Graphics.COLOR_WHITE, Graphics.COLOR_BLACK);
        dc.clear();

        var cx = dc.getWidth() / 2;
        var cy = dc.getHeight() / 2;

        dc.drawText(cx, cy - 55, Graphics.FONT_SMALL, "Julight BLE",
            Graphics.TEXT_JUSTIFY_CENTER);
        dc.drawText(cx, cy - 25, Graphics.FONT_MEDIUM, _state,
            Graphics.TEXT_JUSTIFY_CENTER);

        var rssiText = (_rssi == null) ? "RSSI: --" : ("RSSI: " + _rssi + " dBm");
        dc.drawText(cx, cy + 15, Graphics.FONT_SMALL, rssiText,
            Graphics.TEXT_JUSTIFY_CENTER);

        if (_rssi != null) {
            dc.drawText(cx, cy + 40, Graphics.FONT_SMALL,
                "~ " + estimateDistance(_rssi) + " m",
                Graphics.TEXT_JUSTIFY_CENTER);
        }
    }

    // Crude log-distance path-loss estimate.
    // txPower = expected RSSI at 1 m; n = environmental factor (2.0 = free space).
    // This is a rough proxy only - smoothing/calibration comes later.
    private function estimateDistance(rssi) {
        var txPower = -59.0;
        var n = 2.0;
        var d = Math.pow(10, (txPower - rssi) / (10.0 * n));
        return Math.round(d * 10) / 10.0;
    }
}
