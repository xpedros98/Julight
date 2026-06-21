using Toybox.WatchUi;
using Toybox.Graphics;

// Initial screen: title + a single "Connect a device" button.
class MainView extends WatchUi.View {

    function initialize() {
        View.initialize();
    }

    function onUpdate(dc) {
        dc.setColor(Graphics.COLOR_WHITE, Graphics.COLOR_BLACK);
        dc.clear();

        var cx = dc.getWidth() / 2;
        var cy = dc.getHeight() / 2;

        dc.drawText(cx, cy - 60, Graphics.FONT_MEDIUM, "Julight",
            Graphics.TEXT_JUSTIFY_CENTER);

        // Button
        var bw = (dc.getWidth() * 3) / 4;
        var bh = 52;
        var bx = cx - (bw / 2);
        var by = cy - (bh / 2) + 10;

        dc.setColor(Graphics.COLOR_BLUE, Graphics.COLOR_TRANSPARENT);
        dc.fillRoundedRectangle(bx, by, bw, bh, 12);
        dc.setColor(Graphics.COLOR_WHITE, Graphics.COLOR_TRANSPARENT);
        dc.drawText(cx, by + (bh / 2), Graphics.FONT_SMALL, "Connect a device",
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);

        dc.drawText(cx, dc.getHeight() - 28, Graphics.FONT_XTINY,
            "START / tap to scan", Graphics.TEXT_JUSTIFY_CENTER);
    }
}
