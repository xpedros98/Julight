using Toybox.WatchUi;
using Toybox.System;

class JulightDelegate extends WatchUi.BehaviorDelegate {

    function initialize() {
        BehaviorDelegate.initialize();
    }

    // SELECT/START: send a test payload to the ESP32.
    function onSelect() {
        if (gBle != null) {
            var ok = gBle.sendData([0xDE, 0xAD, 0xBE, 0xEF]b);
            System.println("test send -> " + ok);
        }
        return true;
    }

    // BACK: exit the app.
    function onBack() {
        System.exit();
        return true;
    }
}
