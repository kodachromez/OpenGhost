import QtQuick
import QtQuick.Controls
import OpenGhost.Cpp
import OpenGhost.ToolCalls

// Ported from Ghosty's native-ghosty/qml/ToolCard.qml (see NOTICE.md) for the
// Tool Calls frontend plugin: the ordinary card, without Ghosty's subagent
// activity panel. `row` is the transcript row (ChatEntry): its roles, toggle(),
// dropped(), frontend and list view.
//
// One tool call as OpenGhost's card (.approval.is-tool: the approval card's
// shell and Ghosty's tool layer). Collapsed, its head shows the tool's icon,
// name and humanised arguments, the status and a chevron; the head toggles it.
// Open, the head shows what the call does and the body its details, output
// and how it ended. Arguments are the model's untrusted JSON: only strings are
// shown, nothing is interpreted.
Item {
    id: card
    objectName: "toolCard"
    required property Item row // Its ChatEntry.
    readonly property bool open: row.expanded
    // tool-card.js describe(): what the model asked for, in the card's
    // fields (toolcard.h; the selection copies the same strings).
    readonly property var info: ToolText.describe(row.toolName, row.arguments, row.argumentsKnown)
    readonly property string callState: row.messageState
    width: row.column
    // .approval: 14 px above and around, 12 px below the body. Shut, the
    // head row keeps 14 px below too, so it sits at the card's centre.
    implicitHeight: 46 + (open ? body.height + 12 : 14)

    // The run's own call waiting for the reader's answer (ApprovalCard):
    // it has not started.
    readonly property bool waiting: callState === "running" && !!row.frontend
        && row.frontend.approvals.some(a => a.toolCallId === row.toolCallId)
    readonly property string statusText: ({
        running: waiting ? "Waiting for approval" : "Running…",
        done: "✓ Done",
        error: "✕ Failed",
        cancelled: "Cancelled",
        missing: "No result was saved",
        unconfirmed: "Result unconfirmed"
    })[callState] ?? callState
    readonly property color doneTint: Theme.success
    // How the call ended, as rows of the end well: a Bash result's ending
    // ("exited 0" and any failure or uncertainty), else "Done" or "Failed",
    // else the status.
    readonly property var endLines: ToolText.endLines(callState, row.ending)

    // The output well follows its end while the reader leaves it there
    // (tool-card.js): live output and a Bash result open at their end, any
    // other result at its start. The reader's place is kept while the card
    // is shut, and forgotten with the output.
    readonly property bool tailOutput: (callState !== "done" && callState !== "error")
                                       || row.toolName === "bash" || row.toolName === "bash_output"
                                       || row.toolName === "Bash" || row.toolName === "BashOutput"
    property bool followOutput: true
    property real outputScroll: 0
    readonly property bool hasOutput: row.body.length > 0
    onHasOutputChanged: if (!hasOutput) {
        followOutput = true
        outputScroll = 0
    }
    // Whole lines dropped, then the dropped start of a partial first line.
    readonly property string omission: ToolText.omission(row.omittedLines, row.omittedCharacters)

    // 0 10px 30px rgba(0, 0, 0, .22), then the 1 px rgb(34) inset.
    BoxShadow {
        anchors.fill: parent
        radius: 18
        blur: 30
        offsetY: 10
        color: Theme.alpha("black", 0.22 * Theme.shadow)
    }
    Rectangle {
        anchors.fill: parent
        radius: 18
        antialiasing: true
        color: Theme.composerBg
        border.width: 1
        border.color: Theme.composerBorder
    }

    // A single line of .approval text: its CSS line box, the text centred in it.
    component Line: Label {
        property real box: 20
        height: box
        lineHeightMode: Text.FixedHeight
        lineHeight: box
        topPadding: Theme.halfLeading(font, box)
        bottomPadding: -topPadding
        textFormat: Text.PlainText
        elide: Text.ElideRight
        maximumLineCount: 1
        color: Theme.text
        font.weight: Theme.weight(500)
    }
    // The inset well (.approval-code, .approval-diff): black .28, radius 10,
    // a white .05 inset, scrolling past its maximum height (184 px, or 232
    // for a diff). While it scrolls, Chromium's thin scroll bar takes 10 px
    // of its width from the text.
    component Well: Flickable {
        id: well
        property real most: 184
        property real natural: 0 // The content's height, padding included.
        readonly property bool scrolls: natural > most + 0.5
        // The furthest scroll. Handlers read end(): Flickable moves the
        // content within a new size before this binding catches up.
        readonly property real limit: Math.max(0, contentHeight - height)
        function end() { return Math.max(0, contentHeight - height) }
        width: parent ? parent.width : 0
        height: Math.min(most, natural)
        contentWidth: width - (scrolls ? 10 : 0)
        contentHeight: natural
        clip: scrolls
        interactive: scrolls
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        // A shorter content never leaves the view past its end.
        onLimitChanged: if (contentY > end()) contentY = end()
        // A scroll begun over the well while it can move that way moves it
        // alone, the transcript staying put, until the scroll ends.
        WellScroll {
            target: well
        }
        Rectangle {
            parent: well
            z: -1
            anchors.fill: parent
            radius: 10
            antialiasing: true
            color: Theme.wellBg
            WellRing {}
        }
        WellBar {
            parent: well
            view: well
        }
    }
    // Chromium's default thin scroll bar for the theme (the wells set no
    // scrollbar-color): a 10 px opaque track over the well's edge, square
    // at its corners, a 3 px arrow in each 12 px end, and between them a
    // 6 px round thumb on whole pixels, as long as the visible share of the
    // track. An arrow scrolls 40 px, the track most of a view, and the
    // thumb drags.
    component WellBar: Item {
        id: bar
        objectName: "wellScrollBar"
        required property Flickable view
        readonly property color ink: Theme.light ? Qt.rgba(139 / 255, 139 / 255, 139 / 255, 1)
                                                 : Qt.rgba(159 / 255, 159 / 255, 159 / 255, 1)
        readonly property real track: height - 24
        readonly property real length: Math.max(18, Math.round(track * Math.min(1, view.height / Math.max(1, view.contentHeight))))
        readonly property real at: 12 + Math.round((view.limit > 0 ? view.contentY / view.limit : 0) * (track - length))
        visible: view.scrolls
        z: 1
        x: view.width - 10
        width: 10
        height: view.height
        Rectangle {
            anchors.fill: parent
            color: Theme.light ? Qt.rgba(252 / 255, 252 / 255, 252 / 255, 1)
                               : Qt.rgba(44 / 255, 44 / 255, 44 / 255, 1)
        }
        Rectangle {
            objectName: "wellThumb"
            x: 2
            y: bar.at
            width: 6
            height: bar.length
            radius: 3
            antialiasing: true
            color: bar.ink
        }
        // The arrows, row by row: 2, 4 and 6 px wide.
        Repeater {
            model: 6
            Rectangle {
                required property int index
                readonly property int step: index % 3 // 0 at the tip.
                x: 4 - step
                y: index < 3 ? 4 + step : bar.height - 5 - step
                width: 2 + 2 * step
                height: 1
                color: bar.ink
            }
        }
        MouseArea {
            anchors.fill: parent
            property real grab: -1 // Pointer offset into the thumb while dragging.
            function scrollTo(y) {
                bar.view.contentY = Math.max(0, Math.min(bar.view.end(), y))
            }
            onPressed: mouse => {
                if (mouse.y < 12)
                    scrollTo(bar.view.contentY - 40)
                else if (mouse.y > height - 12)
                    scrollTo(bar.view.contentY + 40)
                else if (mouse.y >= bar.at && mouse.y <= bar.at + bar.length)
                    grab = mouse.y - bar.at
                else
                    scrollTo(bar.view.contentY + (mouse.y < bar.at ? -1 : 1) * bar.view.height * 0.875)
            }
            onPositionChanged: mouse => {
                if (grab >= 0 && bar.track > bar.length)
                    scrollTo((mouse.y - grab - 12) / (bar.track - bar.length) * bar.view.end())
            }
            onReleased: grab = -1
            onCanceled: grab = -1
        }
    }
    // A text of the card's body: one text of the conversation's selection
    // (selection.h, its `unit` toolcard::units()' path), painting its part
    // of the range under its glyphs. The pointer is the card's SelectArea.
    component Selectable: LiveText {
        id: selectable
        property string unit: ""
        function enroll() {
            if (unit.length > 0)
                Selection.enroll(selectable, card.row.key, unit)
        }
        Component.onCompleted: enroll()
        onUnitChanged: enroll()
        padding: 0
        frontend: card.row.frontend
        selectByMouse: false
        activeFocusOnPress: false
        background: SelectionWash {
            target: selectable
            range: { void Selection.revision; return Selection.range(card.row.key, selectable.unit) }
            color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
        }
    }
    // .approval-code's text: 12.5/19 mono at white .88, wrapping anywhere.
    component Code: Selectable {
        x: 12
        y: 8 // 9 px down; 12.5 px mono sits 1 px higher in Chromium.
        width: parent.width - 24
        wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
        color: Theme.alpha(Theme.strong, 0.88)
        font.family: Theme.mono
        font.pointSize: Theme.points(12.5)
        font.weight: Font.Medium
        lineHeight: 19
    }
    // .approval-more: a quiet 12 px note under the details or output, on
    // one 20 px line 3 px down.
    component More: Item {
        id: more
        property alias text: moreText.content
        property alias textUnit: moreText.unit
        height: 23
        Selectable {
            id: moreText
            x: 24
            y: 3
            width: Math.max(0, more.width - 36)
            lineHeight: 20
            wrapMode: TextEdit.NoWrap
            color: Theme.tertiary
            font.pixelSize: 12
            font.weight: Theme.weight(500)
        }
    }
    // One .approval-line: a sign column (none for a new line) and its text.
    component DiffLine: Rectangle {
        id: diffLine
        objectName: "diffLine"
        readonly property int lineCount: lineText.lineCount
        property string sign
        property string text
        property string textUnit
        property string tone: "new" // new, added or removed
        width: parent ? parent.width : 0
        height: Math.max(1, lineText.lineCount) * 19 // Whole CSS line boxes.
        color: tone === "added" ? Theme.alpha(Theme.success, 0.09)
             : tone === "removed" ? Theme.alpha(Theme.danger, 0.09) : "transparent"
        readonly property color ink: tone === "added" ? Theme.diffAdded
                                   : tone === "removed" ? Theme.diffRemoved
                                   : Theme.text
        // The sign is not text to select (.approval-sign: user-select: none).
        Label {
            x: 8
            visible: diffLine.tone !== "new"
            height: 19
            lineHeightMode: Text.FixedHeight
            lineHeight: 19
            topPadding: Theme.halfLeading(font, 19) - 1 // As Code's.
            text: diffLine.sign
            color: diffLine.tone === "added" ? card.doneTint
                 : diffLine.tone === "removed" ? Theme.danger : Theme.tertiary
            font: lineText.font
        }
        Selectable {
            id: lineText
            unit: diffLine.textUnit
            x: diffLine.tone === "new" ? 12 : 24
            y: -1 // As Code's.
            width: diffLine.width - x - 12
            lineHeight: 19
            content: diffLine.text || " "
            wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
            color: diffLine.ink
            font.family: Theme.mono
            font.pointSize: Theme.points(12.5)
            font.weight: Font.Medium
        }
    }

    // .approval-head (<summary>): the whole head toggles the card.
    AbstractButton {
        id: head
        objectName: "toggle"
        x: 14
        y: 14
        width: parent.width - 28
        height: 32
        focusPolicy: Qt.TabFocus
        Accessible.name: card.info.title
        onClicked: card.row.toggle()
        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
        // The 2 px focus outline, 4 px out.
        background: Rectangle {
            x: -6
            y: -6
            width: head.width + 12
            height: head.height + 12
            radius: 10
            visible: head.visualFocus
            color: "transparent"
            border.width: 2
            border.color: Theme.alpha(Theme.strong, 0.35)
        }
        // Every part of the head shares the icon tile's centre, as the
        // 1.1 head's align-items: center does. (1.2's flex-start is for
        // an approval's multi-line main column; on a tool card's single
        // row it leaves the text, status and chevron at the top.)
        contentItem: Item {
            // .approval-icon: the kind's glyph on a white .12 tile.
            Rectangle {
                objectName: "toolIcon"
                anchors.verticalCenter: parent.verticalCenter
                width: 32
                height: 32
                radius: 10
                antialiasing: true
                color: Theme.alpha(Theme.strong, 0.12)
                // Centred at 7.5 px, which Chromium snaps to 8.
                PathIcon {
                    x: 8
                    y: 8
                    width: 17
                    height: 17
                    name: card.info.kind === "file" ? "file" : card.info.kind === "web" ? "globe" : "terminal"
                    color: Theme.strong
                }
            }
            // Collapsed: the capitalised name, then the humanised arguments.
            Line {
                id: toolName
                objectName: "toolName"
                anchors.verticalCenter: parent.verticalCenter
                x: 44
                visible: !card.open
                width: Math.min(implicitWidth, head.width * 0.35)
                text: {
                    const name = card.row.toolName || "Tool"
                    return name[0].toUpperCase() + name.slice(1)
                }
                font.pixelSize: 14
                font.weight: Theme.weight(600)
            }
            Line {
                objectName: "toolSummary"
                anchors.verticalCenter: parent.verticalCenter
                x: toolName.x + toolName.width + 12
                visible: !card.open
                width: Math.max(0, status.x - 12 - x)
                topPadding: Theme.halfLeading(font, box) - 1 // As Code's.
                text: card.info.summary
                color: Theme.secondary
                font.family: Theme.mono
                font.pointSize: Theme.points(12.5)
                font.weight: Font.Medium
            }
            // Open: what the call does, and where.
            Line {
                id: title
                objectName: "toolTitle"
                anchors.verticalCenter: parent.verticalCenter
                x: 44
                visible: card.open
                box: 22
                width: Math.min(implicitWidth, status.x - 12 - x)
                text: card.info.title
                font.pixelSize: 15
                font.weight: Theme.weight(600)
            }
            Line {
                objectName: "toolPath"
                anchors.verticalCenter: parent.verticalCenter
                x: title.x + title.width + 12
                visible: card.open && text.length > 0
                width: Math.max(0, Math.min(implicitWidth, status.x - 12 - x))
                text: card.info.path ?? ""
                font.pixelSize: 14
            }
            Line {
                id: status
                objectName: "toolState"
                anchors.verticalCenter: parent.verticalCenter
                x: head.width - 26 - width
                width: implicitWidth
                // 12 px text sits 1 px higher in Chromium's whole-pixel metrics.
                topPadding: Theme.halfLeading(font, box) - 1
                text: card.statusText
                color: card.callState === "done" ? card.doneTint
                     : card.callState === "error" ? Theme.danger : Theme.tertiary
                font.pixelSize: 12
            }
            // .tool-disclosure: a 6 px chevron, right while shut, down while open.
            Item {
                objectName: "toolDisclosure"
                anchors.verticalCenter: parent.verticalCenter
                x: head.width - 10
                width: 6
                height: 6
                rotation: card.open ? 45 : -45
                Behavior on rotation {
                    enabled: !Theme.reducedMotion
                    NumberAnimation {
                        duration: 200
                        easing.type: Easing.Bezier
                        easing.bezierCurve: Theme.motion
                    }
                }
                Rectangle {
                    x: 4.5
                    width: 1.5
                    height: 6
                    color: Theme.tertiary
                }
                Rectangle {
                    y: 4.5
                    width: 4.5
                    height: 1.5
                    color: Theme.tertiary
                }
            }
        }
    }

    // The details, then the output and how the call ended; nothing is laid
    // out while the card is shut.
    Column {
        id: body
        x: 14
        y: 46
        width: parent.width - 28
        visible: card.open
        Loader {
            width: parent.width
            active: card.open
            sourceComponent: Column {
                width: parent ? parent.width : 0
                Well {
                    objectName: "toolWell"
                    visible: !!card.info.code
                    natural: code.height - ((card.info.code ?? "").endsWith("\n") ? 19 : 0) + 18
                    Code {
                        id: code
                        objectName: "toolCode"
                        unit: "t/c:t"
                        content: card.info.code ?? ""
                    }
                }
                Well {
                    objectName: "toolWell"
                    visible: card.info.removedLines.shown.length + card.info.addedLines.shown.length > 0
                    most: 232
                    natural: diff.height + 12
                    Column {
                        id: diff
                        y: 6
                        width: parent.width
                        Repeater {
                            model: card.info.removedLines.shown
                            DiffLine {
                                required property string modelData
                                required property int index
                                sign: "−"
                                text: modelData
                                textUnit: "t/r" + index + ":t"
                                tone: "removed"
                            }
                        }
                        More {
                            visible: card.info.removedLines.more > 0
                            width: parent.width
                            textUnit: "t/rm:t"
                            text: card.info.removedLines.more + " more lines"
                        }
                        Repeater {
                            model: card.info.addedLines.shown
                            DiffLine {
                                required property string modelData
                                required property int index
                                sign: card.info.removed ? "+" : ""
                                text: modelData
                                textUnit: "t/a" + index + ":t"
                                tone: card.info.removed ? "added" : "new"
                            }
                        }
                        More {
                            visible: card.info.addedLines.more > 0
                            width: parent.width
                            textUnit: "t/am:t"
                            text: card.info.addedLines.more + " more lines"
                        }
                    }
                }
                Selectable {
                    objectName: "toolQuery"
                    visible: !!card.info.text
                    unit: "t/q:t"
                    width: parent.width
                    lineHeight: 20
                    content: card.info.text ?? ""
                    wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
                    color: Theme.text
                    font.pixelSize: 14
                    font.weight: Theme.weight(500)
                }
                Selectable {
                    objectName: "toolLink"
                    visible: card.info.link.length > 0
                    unit: "t/l:t"
                    width: parent.width
                    lineHeight: 20
                    content: card.info.link
                    wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
                    color: Theme.text
                    font.pixelSize: 14
                    font.weight: Theme.weight(500)
                }
                // Lines a live tail dropped from the output's front. The
                // note moves the output as it comes and goes; the transcript
                // keeps the reader's line where it was, as for a trim.
                More {
                    objectName: "omission"
                    visible: text.length > 0
                    width: parent.width
                    textUnit: "t/o:t"
                    text: card.omission
                    property bool built: false
                    Component.onCompleted: built = true
                    onVisibleChanged: if (built && output.visible)
                                          card.row.dropped(output, visible ? -height : height)
                }
                // The output scrolls past 184 px. At its end it follows new
                // output; scrolled back, the reader's line stays put while
                // output streams and its front is trimmed. Until it scrolls,
                // the transcript keeps the reader's place instead.
                Well {
                    id: outputWell
                    objectName: "outputWell"
                    visible: card.hasOutput
                    // A pre's final newline starts no line of its own.
                    natural: output.height - (card.row.body.endsWith("\n") ? 19 : 0) + 18
                    property bool placing: false
                    function place(y) {
                        placing = true
                        contentY = Math.max(0, Math.min(end(), y))
                        placing = false
                        card.outputScroll = contentY
                    }
                    function follow() {
                        if (card.tailOutput && card.followOutput)
                            place(end())
                    }
                    // Opened: at the end it follows, or where the reader left it.
                    function restore() {
                        place(card.tailOutput && card.followOutput ? end() : card.outputScroll)
                    }
                    // Once laid out (Qt.callLater in Ghosty), by a timer
                    // the well owns: a card shut or removed first never runs it.
                    Timer {
                        interval: 0
                        running: true
                        onTriggered: outputWell.restore()
                    }
                    onLimitChanged: follow()
                    // The reader's scrolling, or a shorter output's end; not
                    // the content moving while the text is being edited.
                    onContentYChanged: if (!placing && scrolls && !output.syncing) {
                        card.outputScroll = contentY
                        card.followOutput = end() - contentY < 8
                    }
                    Code {
                        id: output
                        objectName: "output"
                        unit: "t/b:t"
                        content: card.row.body
                        trimmed: card.row.trimmed
                        onDropped: height => {
                            if (!outputWell.scrolls)
                                card.row.dropped(output, height)
                            else if (card.tailOutput && card.followOutput)
                                outputWell.place(outputWell.end())
                            else
                                outputWell.place(card.outputScroll - height)
                        }
                    }
                }
                More {
                    objectName: "toolLive"
                    visible: text.length > 0
                    width: parent.width
                    textUnit: "t/v:t"
                    text: ToolText.liveNote(card.callState, card.row.body.length > 0)
                }
                // How the call ended, as diff rows: done and failed in the
                // added and removed colours, anything else plain.
                Rectangle {
                    objectName: "toolEnd"
                    visible: card.callState !== "running"
                    width: parent.width
                    height: endRows.height + 12
                    radius: 10
                    antialiasing: true
                    color: Theme.wellBg
                    WellRing {}
                    Column {
                        id: endRows
                        y: 6
                        width: parent.width
                        Repeater {
                            model: card.endLines
                            DiffLine {
                                required property string modelData
                                required property int index
                                objectName: "toolEndLine"
                                sign: index > 0 ? "" : card.callState === "done" ? "✓" : card.callState === "error" ? "✕" : ""
                                text: modelData
                                textUnit: "t/e" + index + ":t"
                                tone: card.callState === "done" ? "added" : card.callState === "error" ? "removed" : "new"
                            }
                        }
                    }
                }
            }
        }
    }

    // The pointer over the card's texts, as over a message's (SelectArea):
    // a press starts the conversation's one selection, a drag carries it
    // through the card's details, output, errors and ending and on across
    // the rows around it, up or down. Presses on the head (the toggle: its
    // text is user-select: none in OpenGhost) and the wells' scroll bars
    // pass through; the wheel scrolls a well.
    SelectArea {
        objectName: "selectArea"
        anchors.fill: parent
        z: 1
        host: card.row
        content: card
        view: card.row.ListView.view
    }

    // inset 0 0 0 1px rgba(fg, .05): a ring over the translucent well, not
    // a border that replaces the well's fill under it.
    component WellRing: Rectangle {
        anchors.fill: parent
        radius: 10
        antialiasing: true
        color: "transparent"
        border.width: 1
        border.color: Theme.alpha(Theme.strong, 0.05)
    }
}
