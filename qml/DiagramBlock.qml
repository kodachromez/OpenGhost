import QtQuick
import QtQuick.Controls
import OpenGhost.Cpp

// A drawing (diagram.js DiagramView): compiled and painted off the GUI thread
// (DiagramImage), placed in a stage as wide as the chat the way viewSpec
// places it, coming in item by item when it is built fresh and springing to
// its new layout as it streams, with Edit and Copy on hover. The text is
// authoritative, never the drawing: the editor's text, even empty or not
// parsing, is kept as this presentation's edit as it is typed (never sent),
// and Copy, Done and reopening use it; Undo restores the message's source.
// The drawing follows a pause in typing and keeps the last good version,
// with a note, while the text does not parse. A source that never draws is
// shown as code. A drawing cut off by the display bound (`clipped`) draws
// what is shown but offers no Edit or Copy of a partial source.
Item {
    id: diagram
    objectName: "diagramBlock"
    required property string source
    required property string lang
    required property bool live
    property bool clipped: false
    required property var tones
    required property color tone
    // The section's tone index (its heading's colour) the drawing leads with.
    property int section: -1
    // The text column inside the stage (the stage is the chat's wide width).
    property real column: width
    // Built in a reply as it arrives: its first drawing comes in.
    property bool fresh: false
    required property string path
    property var host: null
    // Its text in the reply's selection (selection.h): its drawn labels,
    // painted where they are drawn.
    property string unit: ""
    // An edit made here (a string, possibly empty), kept by the row across
    // delegate recreation; undefined while the message's source is shown.
    property var edit: host ? host.diagramEdit(path) : undefined
    readonly property bool edited: typeof edit === "string"
    readonly property string shown: edited ? edit : source
    property bool editing: false
    property string preview // What the drawing shows while editing.
    readonly property bool failed: !image.ok && !image.pending && image.error.length > 0 && !live
    // The text does not draw; an older drawing stands.
    readonly property bool stale: image.ok && !image.pending && image.error.length > 0
                                  && image.source === shown

    implicitHeight: failed ? fallback.implicitHeight
                  : stage.height + (note.visible ? note.implicitHeight + 6 : 0)
                    + (editor.active ? editor.height + 8 : 0)

    Connections {
        target: diagram.host
        function onEditsChanged() { diagram.edit = diagram.host.diagramEdit(diagram.path) }
    }

    Item {
        id: stage
        objectName: "diagramStage"
        visible: !diagram.failed
        width: diagram.width
        // The stage springs from the status's height to the drawing's.
        height: image.stageHeight
        clip: true

        // "Drawing diagram" until the first drawing (.md-diagram-status):
        // 12.5 px at 500 in --fg-secondary, breathing to --fg-tertiary and
        // back every 1.6 s, after three 5 px dots 10 px apart in the
        // foreground at .7, .4 and .2, whose last two brighten in turn every
        // 1.2 s (dg-dots). It leaves as the drawing comes (hideStatus: out to
        // nothing and a 4 px blur in 260 ms, at once under reduced motion or
        // when the drawing shows at once).
        Item {
            id: statusHost
            width: stage.width
            height: 56
            property bool gone: image.ok
            opacity: 1
            visible: opacity > 0
            states: State {
                name: "gone"
                when: statusHost.gone
                PropertyChanges { statusHost.opacity: 0 }
            }
            transitions: Transition {
                to: "gone"
                NumberAnimation {
                    property: "opacity"
                    duration: Theme.reducedMotion || !diagram.fresh ? 0 : 260
                    easing.type: Easing.OutQuad
                }
            }
            Row {
                id: status
                anchors.centerIn: parent
                spacing: 12
                property real dots: 0
                property real breath: 0
                NumberAnimation on dots {
                    running: statusHost.visible
                    from: 0
                    to: 1
                    duration: 1200
                    loops: Animation.Infinite
                }
                NumberAnimation on breath {
                    running: statusHost.visible
                    from: 0
                    to: 1
                    duration: 1600
                    loops: Animation.Infinite
                }
                // A keyframe track at `t`: [at, value] pairs, each step eased.
                function track(keys, t) {
                    for (let i = 1; i < keys.length; ++i) {
                        if (t <= keys[i][0]) {
                            const f = (t - keys[i - 1][0]) / (keys[i][0] - keys[i - 1][0])
                            const e = Theme.bezier(0.42, 0, 0.58, 1, f)
                            return keys[i - 1][1] + (keys[i][1] - keys[i - 1][1]) * e
                        }
                    }
                    return keys[keys.length - 1][1]
                }
                Item {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 25 // The dot and its 20 px margin.
                    height: 5
                    Repeater {
                        model: 3
                        delegate: Rectangle {
                            required property int index
                            readonly property var keys: [[[0, 0.7]],
                                                         [[0, 0.4], [0.33, 0.7], [0.66, 0.4], [1, 0.4]],
                                                         [[0, 0.2], [0.33, 0.4], [0.66, 0.7], [1, 0.2]]][index]
                            x: 10 * index
                            width: 5
                            height: 5
                            radius: 2.5
                            color: Theme.alpha(Theme.fg, status.track(keys, status.dots))
                        }
                    }
                }
                Label {
                    objectName: "diagramStatus"
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Drawing diagram"
                    color: Theme.alpha(Theme.fg, status.track([[0, Theme.secondary.a], [0.5, Theme.tertiary.a],
                                                               [1, Theme.secondary.a]], status.breath))
                    font.pointSize: Theme.points(12.5)
                    font.weight: Theme.weight(500)
                }
            }
        }

        // A drawing wider than its stage at its smallest scrolls sideways.
        Flickable {
            id: scroller
            anchors.fill: parent
            contentWidth: Math.max(width, image.stageWidth)
            contentHeight: height
            interactive: contentWidth > width + 1
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds
            DiagramImage {
                id: image
                objectName: "diagram"
                source: diagram.editing ? diagram.preview : diagram.shown
                lang: diagram.lang
                live: diagram.live
                tone: diagram.section
                availableWidth: diagram.width
                column: Math.min(diagram.width, diagram.column)
                animate: diagram.fresh
                reducedMotion: Theme.reducedMotion
                width: Math.max(diagram.width, stageWidth)
                height: stageHeight
                visible: ok
                Component.onCompleted: if (diagram.host && diagram.host.key !== undefined && diagram.unit.length > 0)
                    Selection.enroll(image, diagram.host.key, diagram.unit)
                SelectionWash {
                    objectName: "wash"
                    anchors.fill: parent
                    target: image
                    range: {
                        void Selection.revision
                        return diagram.host && diagram.host.key !== undefined && diagram.unit.length > 0
                               ? Selection.range(diagram.host.key, diagram.unit) : Qt.point(-1, -1)
                    }
                    color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
                }
            }
        }

        // .dg-rule: a hairline at the probed column, fading down.
        Rectangle {
            id: rule
            readonly property var tip: image.tip
            readonly property bool shownNow: !!tip.shown && tip.mode === "probe"
            x: (tip.x !== undefined ? tip.x : 0) - scroller.contentX
            y: tip.top !== undefined ? tip.top : 0
            width: 1
            height: tip.bottom !== undefined ? Math.max(0, tip.bottom - tip.top) : 0
            opacity: shownNow ? 1 : 0
            visible: opacity > 0
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.alpha(Theme.fg, 0.3) }
                GradientStop { position: 1; color: Theme.alpha(Theme.fg, 0.06) }
            }
            Behavior on opacity { NumberAnimation { duration: Theme.reducedMotion ? 0 : 150 } }
            Behavior on x {
                enabled: rule.opacity > 0 && !Theme.reducedMotion
                NumberAnimation { duration: 220; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
            Behavior on height {
                enabled: rule.opacity > 0 && !Theme.reducedMotion
                NumberAnimation { duration: 220; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
        }

        // .dg-tip: what the pointer reads, over a small mark or by the pointer
        // on a large one, kept inside the drawing; beside the rule on a chart.
        Rectangle {
            id: tipBox
            objectName: "diagramTip"
            readonly property var tip: image.tip
            readonly property bool shownNow: !!tip.shown
            readonly property real stageW: image.stageWidth
            readonly property real stageH: image.stageHeight
            width: Math.max(120, tipColumn.implicitWidth + 22)
            height: tipColumn.implicitHeight + 15
            radius: 11
            color: Theme.tipBg
            border.width: 1
            border.color: Theme.alpha(Theme.fg, 0.08)
            opacity: shownNow ? 1 : 0
            scale: shownNow ? 1 : 0.96
            visible: opacity > 0
            z: 3
            // Where upstream's showTip puts it.
            readonly property point place: {
                const t = tip
                if (!t || t.mode === undefined)
                    return Qt.point(x, y)
                let px, py
                if (t.mode === "mark") {
                    const box = t.box
                    const large = t.large && t.px !== undefined
                    const mid = large ? t.px : box.x + box.width / 2
                    const top = large ? t.py - 6 : box.y
                    const bottom = large ? t.py + 12 : box.y + box.height
                    px = mid - width / 2
                    py = top - height - 10
                    if (py < 0)
                        py = bottom + 10
                    py = Math.min(Math.max(py, 0), Math.max(0, stageH - height))
                } else {
                    px = t.x + 14
                    if (px + width > stageW - 2)
                        px = t.x - 14 - width
                    py = t.top
                }
                return Qt.point(Math.min(Math.max(px, 0), Math.max(0, stageW - width)) - scroller.contentX, py)
            }
            x: place.x
            y: place.y
            Behavior on opacity { NumberAnimation { duration: Theme.reducedMotion ? 0 : 150 } }
            Behavior on scale {
                NumberAnimation { duration: Theme.reducedMotion ? 0 : 250; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
            Behavior on x {
                enabled: tipBox.opacity > 0.01 && !Theme.reducedMotion
                NumberAnimation { duration: 220; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
            Behavior on y {
                enabled: tipBox.opacity > 0.01 && !Theme.reducedMotion
                NumberAnimation { duration: 220; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
            Column {
                id: tipColumn
                x: 11
                y: 7
                Label {
                    visible: text.length > 0
                    text: tipBox.tip.title || ""
                    color: Theme.alpha(Theme.fg, Theme.secondary.a)
                    font.pointSize: Theme.points(12.5)
                    font.weight: Theme.weight(550)
                    bottomPadding: 1
                }
                Repeater {
                    model: tipBox.tip.rows || []
                    delegate: Row {
                        id: tipRow
                        required property var modelData
                        spacing: 8
                        height: 19
                        Rectangle {
                            visible: !!tipRow.modelData.hasColor
                            anchors.verticalCenter: parent.verticalCenter
                            width: tipRow.modelData.mark === "line" ? 10 : 8
                            height: tipRow.modelData.mark === "line" ? 2 : 8
                            radius: tipRow.modelData.mark === "bar" ? 2.5 : tipRow.modelData.mark === "line" ? 1 : 4
                            color: visible ? tipRow.modelData.color : "transparent"
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: tipRow.modelData.name
                            color: Theme.alpha(Theme.fg, Theme.secondary.a)
                            font.pointSize: Theme.points(13.5)
                            font.weight: Theme.weight(550)
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: tipRow.modelData.value
                            color: tipRow.modelData.cls.indexOf("is-up") >= 0 ? Theme.success
                                 : tipRow.modelData.cls.indexOf("is-down") >= 0 ? Theme.danger : Theme.fg
                            font.pointSize: Theme.points(13.5)
                            font.weight: Theme.weight(600)
                            font.features: { "tnum": 1 }
                        }
                    }
                }
            }
        }

        // .dg-tools: on hover or while editing; never while streaming. They
        // come in .9 → 1 with a small overshoot, and go back quietly.
        Rectangle {
            id: tools
            readonly property bool on: image.ok && !diagram.live && !diagram.clipped
                                       && (image.hovering || toolsHover.hovered || diagram.editing)
            x: image.tools.x - scroller.contentX
            y: image.tools.y
            z: 3
            width: 68
            height: 36
            radius: 18
            color: Theme.alpha(Theme.chatBg, 0.7)
            border.width: 1
            border.color: Theme.alpha(Theme.fg, 0.1)
            visible: image.ok && !diagram.live && !diagram.clipped && opacity > 0
            opacity: on ? 1 : 0
            scale: on ? 1 : 0.9
            Behavior on opacity { NumberAnimation { duration: Theme.reducedMotion ? 0 : (tools.on ? 220 : 180) } }
            Behavior on scale {
                NumberAnimation {
                    duration: Theme.reducedMotion ? 0 : (tools.on ? 450 : 300)
                    easing.type: Easing.Bezier
                    easing.bezierCurve: tools.on ? [0.34, 1.4, 0.64, 1, 1, 1] : Theme.motion
                }
            }
            HoverHandler { id: toolsHover }
            Row {
                x: 3
                y: 3
                spacing: 2
                ToolButton {
                    id: editButton
                    objectName: "diagramEdit"
                    width: 30
                    height: 30
                    padding: 0
                    enabled: tools.on
                    background: Rectangle {
                        radius: 15
                        color: editButton.hovered ? Theme.alpha(Theme.fg, 0.1) : "transparent"
                    }
                    contentItem: Item {
                        PathIcon {
                            anchors.centerIn: parent
                            width: 15
                            height: 15
                            name: diagram.editing ? "check" : "edit"
                            color: Theme.alpha(Theme.fg, editButton.hovered ? 1 : 0.8)
                        }
                    }
                    onClicked: diagram.editing ? diagram.close() : diagram.open()
                    ToolTip.visible: hovered
                    ToolTip.text: diagram.editing ? "Done editing" : "Edit diagram"
                }
                ToolButton {
                    id: copyButton
                    objectName: "diagramCopy"
                    width: 30
                    height: 30
                    padding: 0
                    enabled: tools.on
                    background: Rectangle {
                        radius: 15
                        color: copyButton.hovered ? Theme.alpha(Theme.fg, 0.1) : "transparent"
                    }
                    contentItem: Item {
                        PathIcon {
                            anchors.centerIn: parent
                            width: 15
                            height: 15
                            name: copied.running ? "check" : "copy"
                            color: copied.running ? Theme.success
                                                  : Theme.alpha(Theme.fg, copyButton.hovered ? 1 : 0.8)
                        }
                    }
                    onClicked: {
                        diagram.copy()
                        copied.restart()
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: "Copy diagram code"
                    Timer {
                        id: copied
                        interval: 1600
                    }
                }
            }
        }
    }

    // .dg-editor: the source, rendered a moment after typing pauses.
    Loader {
        id: editor
        objectName: "diagramEditor"
        active: diagram.editing
        y: stage.height + 8
        width: Math.min(980, diagram.width)
        x: (diagram.width - width) / 2
        sourceComponent: Rectangle {
            width: editor.width
            height: bar.height + codeView.height + (hint.visible ? hint.implicitHeight + 4 : 0) + 12
            radius: 20
            color: Theme.composerBg
            border.width: 1
            border.color: Theme.composerBorder
            Row {
                id: bar
                x: 12
                height: 48
                width: parent.width - 22
                spacing: 6
                layoutDirection: Qt.RightToLeft
                Button {
                    objectName: "diagramDone"
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Done"
                    onClicked: diagram.close()
                }
                Button {
                    objectName: "diagramReset"
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Undo changes"
                    enabled: diagram.edited
                    onClicked: code.text = diagram.source
                }
            }
            ScrollView {
                id: codeView
                x: 12
                y: bar.height
                width: parent.width - 24
                height: Math.min(320, code.implicitHeight + 8)
                TextArea {
                    id: code
                    objectName: "diagramCode"
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.NoWrap
                    selectByMouse: true
                    font.family: Theme.mono
                    font.pixelSize: 13
                    color: Theme.text
                    selectionColor: Theme.selection
                    background: null
                    Component.onCompleted: {
                        text = diagram.shown
                        forceActiveFocus()
                    }
                    onTextChanged: {
                        diagram.keep(text)
                        debounce.restart()
                    }
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Escape
                                || ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                    && (event.modifiers & Qt.ControlModifier))) {
                            event.accepted = true
                            diagram.close()
                        } else if (event.key === Qt.Key_Tab && !(event.modifiers & Qt.ShiftModifier)) {
                            event.accepted = true
                            code.insert(code.cursorPosition, "  ")
                        }
                    }
                }
            }
            Label {
                id: hint
                objectName: "diagramHint"
                x: 14
                y: codeView.y + codeView.height + 4
                width: parent.width - 28
                visible: diagram.stale
                text: "Can’t read this yet, showing the last working version"
                color: Theme.danger
                font.pointSize: Theme.points(12.5)
                wrapMode: Text.Wrap
            }
        }
    }
    // A committed text that does not draw, outside the editor; or a source
    // cut off by the display bound.
    Label {
        id: note
        objectName: "diagramNote"
        visible: (diagram.stale || (diagram.clipped && !diagram.failed)) && !diagram.editing
                 && !diagram.live
        y: stage.height + 6
        x: 4
        width: diagram.width - 8
        text: diagram.clipped ? "Cut off at the display limit: copy the message for all of it"
                              : "Can’t read this diagram, showing the last working version"
        color: diagram.clipped ? Theme.tertiary : Theme.danger
        font.pointSize: Theme.points(12.5)
        wrapMode: Text.Wrap
    }

    // diagram.js EDIT.debounce: render after a 90 ms pause in typing.
    Timer {
        id: debounce
        interval: 90
        onTriggered: if (diagram.editing) diagram.preview = diagram.shown
    }

    // The editor's text is the edit as it is typed; the message's own
    // source is no edit.
    function keep(text) {
        const kept = text === source ? undefined : text
        if (host)
            host.setDiagramEdit(path, kept)
        edit = kept
    }
    // A cut-off source is neither copied nor edited as if it were whole.
    function copy() {
        if (host && !clipped)
            host.frontend.copy(shown)
    }
    function open() {
        if (editing || live || clipped)
            return
        preview = shown
        editing = true
    }
    // Everything typed is already kept: the drawing follows the text.
    function close() {
        if (!editing)
            return
        debounce.stop()
        editing = false
    }

    // Never drawn: its source as a code block, readable and copyable.
    Loader {
        id: fallback
        active: diagram.failed
        width: diagram.width
        sourceComponent: CodeBlock {
            width: diagram.width
            source: diagram.shown
            lang: diagram.lang.length > 0 ? diagram.lang.split(" ")[0] : "mermaid"
            tokens: []
            tone: diagram.tone
            clipped: diagram.clipped
            host: diagram.host
        }
    }
}
