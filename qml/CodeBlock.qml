import QtQuick
import QtQuick.Controls
import OpenGhost.Native

// A fenced code block (.md-code): language label, Copy of the exact source,
// and the source itself, highlighted from runs computed with the parse and
// scrolled sideways rather than wrapped. The text is plain; highlighting
// only styles it. A block cut off by the display bound (`clipped`) holds
// only a prefix of its source: it offers no Copy, which would pass for the
// whole, and says to copy the message instead (audit P5-09).
Rectangle {
    id: code
    objectName: "codeBlock"
    required property string source
    required property string lang
    required property var tokens
    required property color tone
    property bool art: false // A box-drawing text diagram.
    property bool clipped: false
    property var host: null
    property double born: 0 // When its block row was made (TextWave).
    // Its block's path in the reply's selection: the label is ":l", the
    // source ":s" (RichDocument::units()).
    property string unitPath: ""
    readonly property bool inReply: !!host && host.key !== undefined && unitPath.length > 0
    readonly property point selected: {
        void Selection.revision
        return inReply ? Selection.range(host.key, unitPath + ":s") : Qt.point(-1, -1)
    }
    Component.onCompleted: if (inReply) Selection.enroll(text, host.key, unitPath + ":s")
    readonly property bool copied: copiedTimer.running

    readonly property bool overflows: view.contentWidth > view.width
    implicitHeight: 38 + view.height + view.padBottom + (overflows ? bar.height : 0)
    radius: 14
    color: Theme.composerBg
    border.width: 1
    border.color: Theme.composerBorder

    Row {
        id: header
        x: 14
        height: 38
        spacing: 8
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 7
            height: 7
            radius: 3.5
            color: code.tone
        }
        SelectableLabel {
            objectName: "codeLang"
            anchors.verticalCenter: parent.verticalCenter
            text: code.lang
            host: code.host
            unit: code.unitPath.length > 0 ? code.unitPath + ":l" : ""
            color: Theme.secondary
            font.pointSize: Theme.points(12.5)
            font.weight: Font.Bold
            font.letterSpacing: 0.25
        }
    }
    Label {
        objectName: "codeClipped"
        visible: code.clipped
        x: header.x + header.width + 12
        width: Math.max(0, code.width - x - 14)
        height: 38
        horizontalAlignment: Text.AlignRight
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideLeft
        text: "Cut off at the display limit: copy the message for all of it"
        textFormat: Text.PlainText
        color: Theme.tertiary
        font.pointSize: Theme.points(12)
    }
    CopyButton {
        id: copy
        objectName: "codeCopy"
        visible: !code.clipped
        x: code.width - width - 6
        y: 5
        copied: code.copied
        tip: "Copy code"
        onClicked: {
            if (code.clipped)
                return
            if (code.host)
                code.host.frontend.copy(code.source)
            copiedTimer.restart()
        }
    }
    Timer {
        id: copiedTimer
        interval: 1600
    }
    // .md-code pre: its padding scrolls with the code, which the card clips.
    Flickable {
        id: view
        readonly property real side: code.art ? 18 : 16
        readonly property real padBottom: code.art ? 18 : 14
        y: code.art ? 42 : 38
        width: code.width
        height: text.height
        contentWidth: Math.max(width, text.implicitWidth + 2 * side)
        contentHeight: height
        clip: code.overflows
        interactive: code.overflows
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        LiveText {
            id: text
            objectName: "code"
            // A text diagram is centred when it fits (.md-code.is-art code).
            x: code.art ? Math.max(view.side, (view.width - implicitWidth) / 2) : view.side
            width: Math.max(view.width - 2 * view.side, implicitWidth)
            padding: 0
            content: code.source
            wrapMode: TextEdit.NoWrap
            color: Theme.alpha(Theme.strong, 0.88)
            // The reply's SelectArea selects; this paints its part.
            selectByMouse: false
            activeFocusOnPress: false
            activeFocusOnTab: false
            background: SelectionWash {
                objectName: "wash"
                target: text
                range: code.selected
                color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
            }
            font.family: Theme.mono
            font.pointSize: Theme.points(13.5)
            lineHeight: 13.5 * (code.art ? 1.22 : 1.6)
            frontend: code.host ? code.host.frontend : null
            wave: codeWave
            TextWave {
                id: codeWave
                objectName: "wave"
                target: text
                active: !!code.host && !!code.host.rich && code.host.rich.revealing
                fresh: active && Date.now() - code.born < 600
                reducedMotion: Theme.reducedMotion
                selected: code.selected.x >= 0
            }
        }
    }
    // Chromium's thin scroll bar (scrollbar-width: thin, scrollbar-color
    // fg .15 on transparent) under the padding, only for code wider than the
    // block: 10 px tall, a 6 px round thumb 3 px down between 12 px ends, and
    // an arrow at each end, all cut by the card's corners. An arrow scrolls
    // 40 px, the track a page, and the thumb drags.
    Item {
        id: bar
        objectName: "codeScrollBar"
        visible: code.overflows
        y: view.y + view.height + view.padBottom
        width: code.width
        height: 10
        readonly property color ink: Theme.alpha(Theme.strong, 0.15)
        readonly property real track: width - 24
        Rectangle {
            id: thumb
            objectName: "codeThumb"
            x: 12 + view.visibleArea.xPosition * bar.track
            y: 3
            width: Math.max(18, view.visibleArea.widthRatio * bar.track)
            height: 6
            radius: 3
            color: bar.ink
        }
        Repeater {
            model: 2
            Canvas {
                required property int index
                x: index ? bar.width - 12 : 0
                width: 12
                height: bar.height
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.save()
                    ctx.beginPath() // The card's outline, in this canvas's frame.
                    ctx.roundedRect(-x, -bar.y, code.width, code.height, code.radius, code.radius)
                    ctx.clip()
                    ctx.fillStyle = bar.ink
                    ctx.beginPath()
                    const base = 6, tip = index ? 9.3 : 2.7
                    ctx.moveTo(base, 2.9)
                    ctx.lineTo(base, 8.3)
                    ctx.lineTo(tip, 5.6)
                    ctx.closePath()
                    ctx.fill()
                    ctx.restore()
                }
                Connections {
                    target: code
                    function onHeightChanged() { requestPaint() }
                    function onWidthChanged() { requestPaint() }
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            property real grab: -1 // Pointer offset into the thumb while dragging.
            function scrollTo(x) {
                view.contentX = Math.max(0, Math.min(view.contentWidth - view.width, x))
            }
            onPressed: mouse => {
                if (mouse.x < 12)
                    scrollTo(view.contentX - 40)
                else if (mouse.x > width - 12)
                    scrollTo(view.contentX + 40)
                else if (mouse.x >= thumb.x && mouse.x <= thumb.x + thumb.width)
                    grab = mouse.x - thumb.x
                else
                    scrollTo(view.contentX + (mouse.x < thumb.x ? -1 : 1) * view.width * 0.875)
            }
            onPositionChanged: mouse => {
                if (grab >= 0)
                    scrollTo((mouse.x - grab - 12) / bar.track * view.contentWidth)
            }
            onReleased: grab = -1
            onCanceled: grab = -1
        }
    }
    CodeFormat {
        document: text.textDocument
        tokens: code.tokens
        tone: code.tone
    }
}
