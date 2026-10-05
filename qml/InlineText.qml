import QtQuick
import QtQuick.Controls
import OpenGhost.Native

// Read-only styled text: markdown's display text, always plain, with char
// formats set by InlineFormat (never HTML). It is one text (`unit`) of its
// reply's selection (selection.h): the reply's SelectArea takes the pointer,
// so a selection runs on across blocks, and this paints its own part of it
// under the glyphs, which keep their colours. A right click asks for the
// selection menu, and a link opens only on a click (SelectArea), through
// the desktop. (A TextArea: a bare TextEdit relayouts a long paragraph far
// more slowly as it grows.)
TextArea {
    id: area
    required property var content
    property var host: null // The row: key, frontend, selection menu.
    // Its path in the reply's selection; empty outside a reply.
    property string unit: ""
    readonly property point selected: {
        void Selection.revision
        return host && host.key !== undefined && unit.length > 0 ? Selection.range(host.key, unit)
                                                                  : Qt.point(-1, -1)
    }
    function enroll() {
        if (host && host.key !== undefined && unit.length > 0)
            Selection.enroll(area, host.key, unit)
    }
    Component.onCompleted: enroll()
    onUnitChanged: enroll()
    property real pixelSize: 16
    property real lineHeight: 1.65 // CSS line-height.
    // While the row's reply reveals, new text waves in (TextWave); the text
    // it starts with too when its block row was just made (`born`, ms).
    property bool waves: true
    property double born: 0

    // Under the text: inline code and link chips' boxes, a chip's globe and
    // links' underlines (InlineFormat.decorations), in the text's frame.
    background: Item {
        Repeater {
            model: format.decorations
            delegate: Item {
                id: mark
                required property var modelData
                readonly property bool lit: modelData.href.length > 0 && area.hoveredLink === modelData.href
                x: modelData.x + area.leftPadding
                y: modelData.y + area.topPadding
                width: modelData.w
                height: modelData.h
                Rectangle { // .markdown code, .link-chip
                    visible: mark.modelData.kind === "code" || mark.modelData.kind === "chip"
                    anchors.fill: parent
                    readonly property real corner: mark.modelData.kind === "chip" ? 8 : 6
                    topLeftRadius: mark.modelData.first ? corner : 0
                    bottomLeftRadius: mark.modelData.first ? corner : 0
                    topRightRadius: mark.modelData.last ? corner : 0
                    bottomRightRadius: mark.modelData.last ? corner : 0
                    color: Theme.alpha(Theme.strong, mark.modelData.kind === "chip" && mark.lit ? 0.14 : 0.08)
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
                PathIcon { // .link-chip-icon: the local globe (link-chip.css).
                    visible: mark.modelData.kind === "globe"
                    anchors.fill: parent
                    name: "link-globe"
                    color: Theme.alpha(Theme.strong, (mark.lit ? 1 : 0.9) * 0.65)
                }
                Rectangle { // text-decoration-color .35, currentColor on hover
                    visible: mark.modelData.kind === "underline"
                    anchors.fill: parent
                    color: Theme.alpha(Theme.link, mark.lit ? 1 : 0.35)
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
            }
        }
        // ::selection: over the chips' boxes, under the glyphs.
        SelectionWash {
            objectName: "wash"
            anchors.fill: parent
            target: area
            range: area.selected
            color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
        }
    }
    leftPadding: 0
    rightPadding: 0
    textFormat: TextEdit.PlainText
    readOnly: true
    selectByMouse: false
    activeFocusOnPress: false
    activeFocusOnTab: false
    wrapMode: TextEdit.Wrap
    color: Theme.text
    font.pointSize: Theme.points(pixelSize)
    font.weight: Theme.weight(Font.DemiBold)
    // Each line is pixelSize × lineHeight tall (InlineFormat), its extra
    // room above the text; CSS splits the room evenly, so the text moves up
    // half of it.
    topPadding: -format.leading / 2
    bottomPadding: format.leading / 2

    // The visual end, not a character added to the document. Depend on the
    // laid-out size as well as the text: styling and reflow can move it.
    readonly property rect endRect: {
        void width; void contentWidth; void contentHeight; void font; void text
        return positionToRectangle(length)
    }
    readonly property alias format: format
    readonly property alias wave: wave
    TextWave {
        id: wave
        objectName: "wave"
        target: area
        active: area.waves && !!area.host && !!area.host.rich && area.host.rich.revealing
        fresh: active && Date.now() - area.born < 600
        reducedMotion: Theme.reducedMotion
        selected: area.selected.x >= 0
    }
    InlineFormat {
        id: format
        wave: wave
        target: area
        content: area.content
        pixelSize: area.pixelSize
        lineHeight: area.lineHeight
        hoveredLink: area.hoveredLink
    }
    HoverHandler {
        cursorShape: area.hoveredLink ? Qt.PointingHandCursor : Qt.IBeamCursor
    }
}
