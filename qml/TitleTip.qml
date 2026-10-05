import QtQuick
import QtQuick.Controls
import QtQuick.Window
import OpenGhost.Native

// Chromium's own tooltip for an element's title (TooltipAura, measured on
// OpenGhost 1.2): once the pointer has rested on the owner for the delay
// (each move starts it again), 10 px right of and 15 px below the pointer,
// slid left to stay on screen and put above the pointer when it would run off
// the bottom; white on black at .8 in a white line, wrapped at half the
// screen's width (800 px at most), whatever the theme or the desktop's font.
// It stays put while the pointer stays on the owner, never times out, and goes
// when the pointer leaves the owner or a key, button or wheel is pressed;
// after a press it waits for the pointer to leave. Like Chromium's, it takes
// no input. Drawn over the window, so the window's edges stand for the
// screen's. One tip serves any number of owners; `objectName` names its
// Wait timer and Text as well.
Rectangle {
    id: tip
    // The item under the pointer, its text (a string, or a function asked
    // when the tip shows) and where the pointer rests on it.
    property Item owner: null
    property var source
    property point at
    property bool held: false
    // False hides it and stops the wait (a page that closes).
    property bool active: true
    parent: Overlay.overlay
    z: 1000000
    visible: false
    width: Math.ceil(tipText.contentWidth) + 18
    height: tipText.lineCount * 19 + 9
    color: Qt.rgba(0, 0, 0, 0.8)
    border.width: 1
    border.color: "white"
    // Qt repeats a hover at the same place as the scene changes under it;
    // only a move starts the wait again.
    function hover(item, text, point) {
        if (owner === item && at.x === point.x && at.y === point.y)
            return
        if (owner !== item) {
            visible = false
            held = false
        }
        owner = item
        source = text
        at = point
        if (!held && !visible)
            wait.restart()
    }
    function leave(item) {
        if (owner !== item)
            return
        owner = null
        held = false
        wait.stop()
        visible = false
    }
    function press() {
        if (!owner)
            return
        held = true
        wait.stop()
        visible = false
    }
    onActiveChanged: if (!active) leave(owner)
    Timer {
        id: wait
        objectName: tip.objectName + "Wait"
        interval: 550
        onTriggered: {
            if (!tip.owner || !tip.active || !tip.parent)
                return
            tipText.text = typeof tip.source === "function" ? tip.source() : tip.source
            if (tipText.text.length === 0)
                return
            const at = tip.owner.mapToItem(tip.parent, tip.at)
            const room = tip.parent
            tip.x = Math.max(0, Math.min(at.x + 10, room.width - tip.width))
            tip.y = at.y + 15 + tip.height > room.height ? Math.max(0, at.y - tip.height) : at.y + 15
            tip.visible = true
        }
    }
    Text {
        id: tipText
        objectName: tip.objectName + "Text"
        // TooltipAura's widest: half the screen, at most 800 px.
        readonly property real widest: Math.min(800, Math.floor(((tip.Screen.width || 1600) + 1) / 2))
        x: 9
        y: 6
        width: Math.min(implicitWidth, widest)
        // Chromium's tooltip face and size, hinted to whole pixels as
        // Chromium draws it.
        font.family: "DejaVu Sans"
        font.pixelSize: 14
        font.hintingPreference: Font.PreferFullHinting
        renderType: Text.NativeRendering
        lineHeightMode: Text.FixedHeight
        lineHeight: 19
        color: "white"
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }
    InputWatch {
        window: tip.Window.window
        onPressed: tip.press()
    }
}
