using Toybox.Application;
using Toybox.WatchUi;

// Global handle to the BLE manager so views/delegates can reach it.
var gBle = null;

class JulightApp extends Application.AppBase {

    function initialize() {
        AppBase.initialize();
    }

    function onStart(state) {
    }

    function onStop(state) {
        if (gBle != null) {
            gBle.shutdown();
        }
    }

    // Start on the main screen with the "Connect a device" button.
    function getInitialView() {
        gBle = new BleManager();
        gBle.start();
        return [new MainView(), new MainDelegate()];
    }
}
