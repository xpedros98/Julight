using Toybox.WatchUi;

// Main screen input: START button or a screen tap begins scanning.
class MainDelegate extends WatchUi.BehaviorDelegate {

    function initialize() {
        BehaviorDelegate.initialize();
    }

    function onSelect() {
        startScan();
        return true;
    }

    function onTap(clickEvent) {
        startScan();
        return true;
    }

    private function startScan() {
        WatchUi.pushView(new ScanningView(), new ScanningDelegate(), WatchUi.SLIDE_LEFT);
    }
}
