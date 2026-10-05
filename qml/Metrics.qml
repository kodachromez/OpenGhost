import QtQuick
import QtQuick.Controls
import OpenGhost.Native

// A run's reply metrics (.reply-metrics): tokens, elapsed and cache hit, as
// the worker computed them ("%1" where the elapsed time goes). Its
// one-second clock runs only while the run is live with a known accepted
// start and the label can be seen (`seen`, from its owner: an exposed window
// and inside its viewport), catching up at once when seen again; a settled
// run's time is bounded only by its saved end, never guessed.
Label {
    id: footer
    objectName: "metrics"
    property string template
    property string tip // Numbers and fixed text only.
    property double started: -1 // ms since the epoch; -1 unknown.
    property double completed: -1
    property bool live: false
    property bool seen: false
    property real gap: 0 // Above the text (margin-top).
    property double now: Date.now()
    readonly property bool ticking: live && started >= 0
    readonly property bool clocking: ticking && seen && visible
    readonly property double end: completed >= 0 ? completed : ticking ? now : -1
    text: template ? template.arg(started >= 0 && end >= started ? elapsed(end - started) : "—")
                   : ""
    textFormat: Text.PlainText
    color: Theme.secondary
    font.pixelSize: 14
    font.features: { "tnum": 1 }
    lineHeightMode: Text.FixedHeight
    lineHeight: 14 * 1.4
    topPadding: gap + Theme.halfLeading(font, lineHeight)
    bottomPadding: -Theme.halfLeading(font, lineHeight)
    function elapsed(ms) {
        const seconds = Math.floor(ms / 1000), minutes = Math.floor(seconds / 60)
        return minutes >= 60 ? Math.floor(minutes / 60) + "h " + minutes % 60 + "m"
             : minutes ? minutes + "m " + seconds % 60 + "s" : seconds + "s"
    }
    onClockingChanged: if (clocking) now = Date.now()
    Timer {
        interval: 1000
        repeat: true
        running: footer.clocking
        onTriggered: footer.now = Date.now()
    }
    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered
    ToolTip.text: tip
}
