import QtQuick
import QtQuick.Controls
import OpenGhost.Cpp

// Ported from Ghosty's native-ghosty/qml/ChatEntry.qml thinking row (see
// NOTICE.md) for the Thinking frontend plugin. `row` is the transcript row
// (ChatEntry): its roles, toggle(), frontend and list view.
//
// The summary and, open, the text: texts of the conversation's selection,
// the pointer over them the SelectArea's (the summary itself toggles, as a
// <summary> does). Shut while it streams, the summary's newest item is all
// that shows of it; open, all of it does. Like Ghosty's, it has no entry
// motion of its own.
Item {
    id: thinking
    objectName: "thinkingEntry"
    required property Item row // Its ChatEntry.
    width: row.column
    implicitHeight: thought.implicitHeight
    SelectArea {
        objectName: "selectArea"
        anchors.fill: parent
        z: 1
        host: thinking.row
        content: thought
        view: thinking.row.ListView.view
    }
    Column {
        id: thought
        width: thinking.row.column
        spacing: 6
        // .message-thinking > summary: Chromium's disclosure triangle
        // and "Thinking"; while it streams, "Thinking…" and its newest
        // item, which only shows the text below and is not selectable.
        AbstractButton {
            id: summary
            objectName: "toggle"
            readonly property bool live: thinking.row.messageState === "live"
            width: latestLabel.x + latestLabel.width
            height: 23.1
            focusPolicy: Qt.TabFocus
            Accessible.name: summaryLabel.text + (latestLabel.text.length > 0 ? " " + latestLabel.text : "")
            onClicked: thinking.row.toggle()
            background: Rectangle {
                x: -5
                y: -5
                width: summary.width + 10
                height: summary.height + 10
                radius: 8
                visible: summary.visualFocus
                color: "transparent"
                border.width: 2
                border.color: Theme.alpha(Theme.strong, 0.35)
            }
            contentItem: Item {
                PathIcon {
                    y: 8
                    width: 9.25
                    height: 9.25
                    name: thinking.row.expanded ? "disclosure-open" : "disclosure-closed"
                    color: Theme.secondary
                }
                Label {
                    id: summaryLabel
                    objectName: "thinkingSummary"
                    x: 15 // After the marker's box and a space: 14.4, which Qt rounds down.
                    height: 23.1
                    text: summary.live ? "Thinking…" : "Thinking"
                    color: Theme.secondary
                    font.pixelSize: 14
                    lineHeightMode: Text.FixedHeight
                    lineHeight: 23.1
                    topPadding: Theme.halfLeading(font, lineHeight)
                    bottomPadding: -topPadding
                    readonly property string unit: "k:s"
                    Component.onCompleted: Selection.enroll(summaryLabel, thinking.row.key, unit)
                    SelectionWash {
                        z: -1
                        anchors.fill: parent
                        target: summaryLabel
                        range: { void Selection.revision; return Selection.range(thinking.row.key, "k:s") }
                        color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
                    }
                }
                Label {
                    id: latestLabel
                    objectName: "thinkingLatest"
                    x: summaryLabel.x + summaryLabel.implicitWidth + 8
                    anchors.baseline: summaryLabel.baseline
                    width: text.length > 0 ? Math.min(implicitWidth, Math.max(0, thinking.row.column - x)) : 0
                    text: summary.live ? thinking.row.preview : ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    color: Theme.secondary
                    font.pixelSize: 14
                }
            }
        }
        // .thinking-content: a 1 px rule, 12 px in, the text as sent.
        Item {
            visible: thinking.row.expanded
            width: parent.width
            height: thinkingText.height
            Rectangle {
                width: 1
                height: parent.height
                color: Theme.quaternary
            }
            LiveText {
                id: thinkingText
                objectName: "thinkingBody"
                frontend: thinking.row.frontend
                visible: thinking.row.expanded
                x: 13
                width: parent.width - 13
                padding: 0
                content: thinking.row.expanded ? thinking.row.body : ""
                wrapMode: TextEdit.Wrap
                color: Theme.secondary
                font.pixelSize: 14
                lineHeight: 14 * 1.65
                // The conversation's selection, not its own.
                selectByMouse: false
                activeFocusOnPress: false
                readonly property string unit: "k:t"
                Component.onCompleted: Selection.enroll(thinkingText, thinking.row.key, unit)
                background: SelectionWash {
                    target: thinkingText
                    range: { void Selection.revision; return Selection.range(thinking.row.key, "k:t") }
                    color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
                }
            }
        }
    }
}
