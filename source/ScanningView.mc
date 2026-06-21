using Toybox.WatchUi;
using Toybox.Graphics;
using Toybox.Timer;

// Scans for ~5 seconds, then switches to the device list.
class ScanningView extends WatchUi.View {

    private const SCAN_MS = 5000;
    private var _timer = null;

    function initialize() {
        View.initialize();
    }

    function onShow() {
        gBle.beginScan();
        _timer = new Timer.Timer();
        _timer.start(method(:onScanDone), SCAN_MS, false);
    }

    function onHide() {
        if (_timer != null) {
            _timer.stop();
            _timer = null;
        }
    }

    function onUpdate(dc) {
        dc.setColor(Graphics.COLOR_WHITE, Graphics.COLOR_BLACK);
        dc.clear();
        var cx = dc.getWidth() / 2;
        var cy = dc.getHeight() / 2;
        dc.drawText(cx, cy - 15, Graphics.FONT_MEDIUM, "Scanning...",
            Graphics.TEXT_JUSTIFY_CENTER);
        dc.drawText(cx, cy + 20, Graphics.FONT_SMALL,
            gBle.getDiscovered().size() + " found",
            Graphics.TEXT_JUSTIFY_CENTER);
    }

    // Timer callback: stop scanning and show the result list.
    function onScanDone() as Void {
        gBle.stopScan();
        WatchUi.switchToView(buildDeviceMenu(), new DeviceMenuDelegate(),
            WatchUi.SLIDE_LEFT);
    }
}
