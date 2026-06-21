using Toybox.WatchUi;

// While scanning, BACK cancels and returns to the main screen.
class ScanningDelegate extends WatchUi.BehaviorDelegate {

    function initialize() {
        BehaviorDelegate.initialize();
    }

    function onBack() {
        gBle.stopScan();
        WatchUi.popView(WatchUi.SLIDE_RIGHT);
        return true;
    }
}
