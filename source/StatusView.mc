using Toybox.WatchUi;
using Toybox.Graphics;

// Shown after a device is selected: connection state, send feedback, and a hint
// for sending data. The ESP32 receives writes here.
class StatusView extends WatchUi.View {

    private var _state = "connecting";
    private var _sent = 0;

    function initialize() {
        View.initialize();
    }

    function onShow() {
        if (gBle != null) {
            gBle.setStatusView(self);
            _state = gBle.connState;
            _sent = gBle.txCount;
        }
    }

    function onHide() {
        if (gBle != null) {
            gBle.setStatusView(null);
        }
    }

    // Called by BleManager when the connection state changes.
    function updateState(state) {
        _state = state;
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

        dc.drawText(cx, cy - 40, Graphics.FONT_SMALL, "Julight BLE",
            Graphics.TEXT_JUSTIFY_CENTER);
        dc.drawText(cx, cy - 12, Graphics.FONT_MEDIUM, _state,
            Graphics.TEXT_JUSTIFY_CENTER);

        dc.drawText(cx, cy + 24, Graphics.FONT_SMALL, "sent: " + _sent,
            Graphics.TEXT_JUSTIFY_CENTER);

        dc.drawText(cx, dc.getHeight() - 26, Graphics.FONT_XTINY,
            "START: send", Graphics.TEXT_JUSTIFY_CENTER);
    }
}
