using Toybox.WatchUi;
using Toybox.Lang;

// Builds a selectable list of the devices found during scanning.
// Each item's id is its index into gBle.getDiscovered(); -1 = "no devices".
function buildDeviceMenu() {
    var menu = new WatchUi.Menu2({ :title => "Devices" });
    var list = gBle.getDiscovered();

    if (list.size() == 0) {
        menu.addItem(new WatchUi.MenuItem("No devices found", "Back to retry", -1, null));
        return menu;
    }

    for (var i = 0; i < list.size(); i++) {
        var e = list[i];
        var label = (e[:name] != null) ? e[:name] : "(no name)";
        var sub = e[:rssi] + " dBm";
        if (e[:uuid] != null) {
            sub = sub + "  " + shortenUuid(e[:uuid]);
        }
        menu.addItem(new WatchUi.MenuItem(label, sub, i, null));
    }
    return menu;
}

// Show just the first block of the UUID so it fits on screen.
function shortenUuid(uuid as Lang.String) {
    if (uuid.length() > 8) {
        return uuid.substring(0, 8) + "...";
    }
    return uuid;
}

class DeviceMenuDelegate extends WatchUi.Menu2InputDelegate {

    function initialize() {
        Menu2InputDelegate.initialize();
    }

    function onSelect(item) {
        var id = item.getId();
        if (id != null && (id as Lang.Number) >= 0) {
            var entry = gBle.getDiscovered()[id];
            gBle.connectTo(entry[:scan]);
            WatchUi.switchToView(new StatusView(), new StatusDelegate(),
                WatchUi.SLIDE_LEFT);
        }
    }

    function onBack() {
        WatchUi.popView(WatchUi.SLIDE_RIGHT);
    }
}
