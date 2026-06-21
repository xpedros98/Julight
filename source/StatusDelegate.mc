using Toybox.WatchUi;
using Toybox.System;

// Status screen input: START sends a test payload, BACK returns to the list.
class StatusDelegate extends WatchUi.BehaviorDelegate {

    function initialize() {
        BehaviorDelegate.initialize();
    }

    function onSelect() {
        if (gBle != null) {
            var ok = gBle.sendData([0xDE, 0xAD, 0xBE, 0xEF]b);
            System.println("test send -> " + ok);
        }
        return true;
    }

    function onBack() {
        WatchUi.popView(WatchUi.SLIDE_RIGHT);
        return true;
    }
}
