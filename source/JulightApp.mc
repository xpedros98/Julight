using Toybox.Application;
using Toybox.WatchUi;

// Global handle to the BLE manager so the input delegate can reach it.
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

    // Returns the initial view + input delegate of the app.
    function getInitialView() {
        var view = new JulightView();
        gBle = new BleManager(view);
        gBle.start();
        var delegate = new JulightDelegate();
        return [view, delegate];
    }
}
