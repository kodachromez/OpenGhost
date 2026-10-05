import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import OpenGhost.Native

// One OpenGhost transcript row. User text and notes are plain text; an
// assistant's text is Markdown shown through the allowlisted components in
// Markdown.qml, parsed off the GUI thread. Plain bodies use LiveText.
Item {
    id: entry
    required property int index
    required property string key
    required property string kind
    required property string body
    required property string preview
    required property string messageState
    required property bool copyable
    required property var attachments // User rows: {name, mime, size} cards.
    // A run's last reply row: its reply metrics and saved bounds (Metrics.qml).
    required property string metrics
    required property double started
    required property double completed
    // How it follows the row before (Entry::Join): messages are 28 px apart
    // (.thread-list); parts of one reply are 14 px apart.
    required property int join

    width: ListView.view ? ListView.view.width : 0
    // The reading column (.thread-list): at most 680 px, 24 px from the edges.
    readonly property real column: Math.min(680, width - 48)
    readonly property real columnX: (width - column) / 2
    readonly property real gap: index === 0 ? 0 : join === 1 ? 14 : 28
    height: gap + (content.item ? content.item.implicitHeight : 0)
    // Its full height when the list places it: a row that grows after it
    // appears makes following the end rebuild the rows around it, and one
    // above the view would move what the reader sees. A row made below the
    // view (scrolling toward the end) instead builds its blocks in the
    // Pacer's turn once placed, growing only the end (audit P6-07).
    Component.onCompleted: {
        const view = ListView.view
        const top = view ? view.itemAt(0, view.contentY) : null
        eager = !top || index < top.index
        listed = eager
        rich.paced = !eager
        rich.hold = false // Parses now, unless paced and past the slice's budget.
        Theme.settle(entry)
        Qt.callLater(entry.near)
        if (view && view.opening)
            arrive()
    }
    objectName: "entry-" + kind

    // The list's viewport and a viewport's height more on each side, in this
    // row's coordinates: the part of a lazy message whose blocks are built
    // (Block.qml), from nearby.x to nearby.y. It moves in steps of half a
    // viewport, so blocks re-check it rarely, and is empty until the list has
    // placed the row (a row is created before it is positioned). It follows
    // the view one event later, never within the view's own scrolling or
    // layout: building blocks there would resize this row while the view moves.
    property point nearby: Qt.point(-1e9, -1e9)
    // Blocks are built once `listed`: at once for a row made above the view
    // or into an empty one (`eager`), else once near() has seen the list
    // position it and which of its blocks are ahead of the window (Block.qml).
    property bool listed: false
    property bool eager: false
    function near() {
        const view = ListView.view
        if (!view) {
            nearby = Qt.point(-Infinity, Infinity)
            eager = false
            listed = true
            return
        }
        const step = Math.max(100, view.height / 2)
        const top = Math.floor((view.contentY - y) / step) * step - 2 * step
        nearby = Qt.point(top, top + 7 * step)
        eager = false
        listed = true
    }
    onYChanged: Qt.callLater(entry.near)
    Connections {
        target: entry.ListView.view
        function onContentYChanged() { Qt.callLater(entry.near) }
        function onHeightChanged() { Qt.callLater(entry.near) }
    }

    function menu(area) {
        entry.ListView.view.showMenu(area)
    }
    readonly property var frontend: ListView.view ? ListView.view.frontend : null
    function dropped(area, height) {
        entry.ListView.view.dropped(entry, area.mapToItem(entry, 0, area.topPadding).y, height)
    }
    // Diagram edits are presentation only, kept by key while the row lives.
    property int edits: 0
    function diagramEdit(path) {
        return entry.ListView.view ? entry.ListView.view.model.edit(entry.key + path) : undefined
    }
    function setDiagramEdit(path, source) {
        entry.ListView.view.model.setEdit(entry.key + path, source)
        edits++
    }

    // An assistant's Markdown. A row that arrives while streaming reveals
    // its text at OpenGhost's pace; one created whole is shown at once.
    RichDocument {
        id: rich
        source: entry.kind === "assistant" ? entry.body : ""
        live: entry.messageState === "live"
        seed: entry.key
        hold: true // Until onCompleted knows whether the row is paced.
        reducedMotion: Theme.reducedMotion
        devicePixelRatio: Screen.devicePixelRatio
    }
    readonly property alias rich: rich
    ListView.onAdd: {
        if (entry.kind === "assistant" && entry.messageState === "live")
            rich.revealFromStart()
        entry.arrive()
    }

    // Entry motion, for a row added while its conversation is shown or shown
    // as it opens, never one built by scrolling: a message rises 8 px in
    // 0.35 s (message-in), and a reply's copy control comes down 4 px in
    // 0.45 s as it appears (message-tools-in). None with reduced motion.
    property real arrival: 1
    property real toolsArrival: 1
    // A Markdown block's own entry motion (md-rise, md-pop, md-bar, …) plays
    // for content built soon after its block row was made (`born`, ms), in a
    // row that arrived this way or while its reply reveals; never for
    // content built again by scrolling.
    property double arrivedAt: -1
    function fresh(born) {
        const now = Date.now()
        return !Theme.reducedMotion && now - born < 600
               && (rich.revealing || now - arrivedAt < 600)
    }
    function arrive() {
        arrivedAt = Date.now()
        if (Theme.reducedMotion)
            return
        if (kind === "user")
            arriving.restart()
        if (copyable)
            toolsArriving.restart()
    }
    onCopyableChanged: if (copyable && !Theme.reducedMotion) toolsArriving.restart()
    NumberAnimation {
        id: arriving
        target: entry
        property: "arrival"
        from: 0
        to: 1
        duration: 350
        easing.type: Easing.Bezier
        easing.bezierCurve: Theme.motion
    }
    NumberAnimation {
        id: toolsArriving
        target: entry
        property: "toolsArrival"
        from: 0
        to: 1
        duration: 450
        easing.type: Easing.Bezier
        easing.bezierCurve: Theme.motion
    }
    Connections {
        target: Theme
        enabled: arriving.running || toolsArriving.running
        function onReducedMotionChanged() {
            arriving.complete()
            toolsArriving.complete()
        }
    }

    Loader {
        id: content
        x: entry.columnX
        y: entry.gap
        width: entry.column
        opacity: entry.arrival
        transform: Translate { y: 8 * (1 - entry.arrival) }
        sourceComponent: entry.kind === "note" ? note
                       : entry.kind === "user" ? userMessage : assistantMessage
    }

    // A message's copy button (.message-tools .md-copy).
    component MessageCopy: CopyButton {
        id: copyButton
        objectName: "copy"
        tip: "Copy message"
        onClicked: {
            entry.frontend.copyEntry(entry.key)
            copied = true
            doneTimer.restart()
        }
        Timer {
            id: doneTimer
            interval: 1600
            onTriggered: copyButton.copied = false
        }
    }

    Component {
        id: userMessage
        // .message.is-user: the files, the bubble and a steering receipt,
        // right-aligned 6 px apart; the copy button hangs below, shown while
        // the message is hovered. Its file cards' texts and the bubble are
        // texts of the conversation's selection (selection.h), as 1.2's DOM
        // has them: the pointer over them is the SelectArea's.
        Item {
            width: entry.column
            implicitHeight: user.height
            HoverHandler { id: userHover }
            SelectArea {
                objectName: "selectArea"
                anchors.fill: parent
                z: 1
                host: entry
                content: user
                view: entry.ListView.view
            }
            // Below the message (top: 100%, 2 px down), out of its height.
            MessageCopy {
                objectName: "userCopy"
                anchors.right: parent.right
                y: user.height + 2
                z: 1
                visible: entry.copyable
                opacity: userHover.hovered || hovered || visualFocus || copied ? 1 : 0
                Behavior on opacity {
                    enabled: !Theme.reducedMotion
                    NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
                }
            }
            Column {
                id: user
                width: entry.column
                spacing: 6
                // The prompt's widest line, unwrapped: the bubble fits it up to
                // 80% of the column (measured, so wrapping cannot feed back).
                readonly property real room: entry.column * 0.8 - 32
                property real natural: 0
                TextMetrics {
                    id: metrics
                    font: messageBody.font
                }
                function measure() {
                    let widest = 0
                    for (const line of entry.body.split("\n")) {
                        metrics.text = line
                        widest = Math.max(widest, metrics.advanceWidth)
                        if (widest >= room)
                            break
                    }
                    natural = Math.ceil(widest) + 6 // The caret and rounding.
                }
                Component.onCompleted: measure()
                Connections {
                    target: entry
                    function onBodyChanged() { user.measure() }
                    function onColumnChanged() { user.measure() }
                }
                // OpenGhost's attachment metadata (name, type, size); never a path.
                // .message-files: cards 8 px apart, wrapping within 80% of the
                // column, each line against the right edge.
                Item {
                    id: files
                    objectName: "sentFiles"
                    width: parent.width
                    visible: entry.attachments.length > 0
                    readonly property real room: entry.column * 0.8
                    onRoomChanged: Qt.callLater(layout)
                    function layout() {
                        let y = 0, line = [], used = 0, tall = 0
                        const place = () => {
                            let x = width
                            for (let k = line.length - 1; k >= 0; --k) {
                                x -= line[k].width
                                line[k].x = x
                                line[k].y = y
                                x -= 8
                            }
                        }
                        for (let i = 0; i < cards.count; ++i) {
                            const card = cards.itemAt(i)
                            if (!card)
                                continue
                            if (line.length > 0 && used + 8 + card.width > room) {
                                place()
                                y += tall + 8
                                line = []
                                used = 0
                                tall = 0
                            }
                            used += (line.length > 0 ? 8 : 0) + card.width
                            tall = Math.max(tall, card.height)
                            line.push(card)
                        }
                        if (line.length > 0) {
                            place()
                            y += tall
                        }
                        height = y
                    }
                    Repeater {
                        id: cards
                        model: entry.attachments
                        onItemAdded: Qt.callLater(files.layout)
                        onItemRemoved: Qt.callLater(files.layout)
                        delegate: Column {
                            id: attachment
                            required property var modelData
                            required property int index
                            // OpenGhost-validated PNG, JPEG and WebP images: a saved
                            // one previews on request, when OpenGhost is idle.
                            readonly property bool picture: ["image/png", "image/jpeg", "image/webp"]
                                                            .indexOf(modelData.mime) >= 0
                            // The facade's preview state and image for this card.
                            // Only a ready entry has an image: in any other
                            // state, evicted included, the card's pixels and
                            // texture go (audit P5-15).
                            property string preview
                            function refresh() {
                                preview = entry.frontend ? entry.frontend.previewState(entry.key, index) : ""
                                if (entry.frontend)
                                    shownPreview.image = entry.frontend.previewImage(entry.key, index)
                            }
                            Component.onCompleted: refresh()
                            onWidthChanged: Qt.callLater(files.layout)
                            onHeightChanged: Qt.callLater(files.layout)
                            spacing: 6
                            Connections {
                                target: entry.frontend
                                function onPreviewChanged(key, card) {
                                    if (key === entry.key && card === attachment.index)
                                        attachment.refresh()
                                }
                            }
                            // .file-card: the kind's icon, the name and "Text · 48 KB";
                            // at most 280 px, and never wider than the files' room.
                            Rectangle {
                                id: fileCard
                                objectName: "sentAttachment"
                                anchors.right: parent.right
                                readonly property bool counted: attachment.modelData.size < 0
                                width: Math.min(280, files.room,
                                                (counted ? 14 : 50) + Math.max(cardName.implicitWidth,
                                                                            cardMeta.implicitWidth) + 14)
                                height: 56
                                radius: 16
                                antialiasing: true
                                color: Theme.composerBg
                                border.width: 1
                                border.color: Theme.composerBorder
                                FileIcon {
                                    id: kind
                                    visible: !fileCard.counted
                                    x: 10
                                    y: 9
                                    width: 30
                                    height: 38
                                    fileName: attachment.modelData.name
                                    // The extension label, an SVG <text> in 1.2.
                                    Item {
                                        id: extension
                                        x: kind.labelRect.x
                                        y: kind.labelRect.y
                                        width: kind.labelRect.width
                                        height: kind.labelRect.height
                                        readonly property string unit: "u/f" + attachment.index + ":i"
                                        Component.onCompleted: if (!fileCard.counted)
                                            Selection.enroll(extension, entry.key, unit)
                                        SelectionWash {
                                            anchors.fill: parent
                                            target: extension
                                            range: { void Selection.revision; return Selection.range(entry.key, extension.unit) }
                                            color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
                                        }
                                    }
                                }
                                Label {
                                    id: cardName
                                    objectName: "sentAttachmentName"
                                    x: fileCard.counted ? 14 : 50
                                    y: fileCard.counted ? 18 : 10
                                    width: fileCard.width - x - 14
                                    height: 19
                                    lineHeightMode: Text.FixedHeight
                                    lineHeight: 19
                                    topPadding: Theme.halfLeading(font, 19)
                                    text: attachment.modelData.name
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: Theme.text
                                    font.pixelSize: 14
                                    font.weight: Theme.weight(600)
                                    readonly property string unit: "u/f" + attachment.index + ":n"
                                    Component.onCompleted: Selection.enroll(cardName, entry.key, unit)
                                    SelectionWash {
                                        z: -1
                                        anchors.fill: parent
                                        target: cardName
                                        range: { void Selection.revision; return Selection.range(entry.key, cardName.unit) }
                                        color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
                                    }
                                }
                                Label {
                                    id: cardMeta
                                    objectName: "sentAttachmentMeta"
                                    visible: !fileCard.counted
                                    x: 50
                                    y: 29
                                    width: fileCard.width - x - 14
                                    height: 17
                                    lineHeightMode: Text.FixedHeight
                                    lineHeight: 17
                                    topPadding: Theme.halfLeading(font, 17)
                                    text: fileCard.counted ? "" : kind.kind + " · "
                                          + (entry.ListView.view ? entry.ListView.view.formatSize(attachment.modelData.size) : "")
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: Theme.secondary
                                    font.pointSize: Theme.points(12.5)
                                    font.weight: Theme.weight(500)
                                    readonly property string unit: "u/f" + attachment.index + ":m"
                                    Component.onCompleted: if (!fileCard.counted)
                                        Selection.enroll(cardMeta, entry.key, unit)
                                    SelectionWash {
                                        z: -1
                                        anchors.fill: parent
                                        target: cardMeta
                                        range: { void Selection.revision; return Selection.range(entry.key, cardMeta.unit) }
                                        color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
                                    }
                                }
                            }
                            ToolButton {
                                objectName: "previewAttachment"
                                anchors.right: parent.right
                                visible: attachment.picture && attachment.preview !== "ready"
                                enabled: attachment.preview !== "loading" && !entry.frontend.busy
                                height: 24
                                padding: 4
                                text: attachment.preview === "loading" ? "Loading…" : "Preview"
                                font.pixelSize: 12
                                onClicked: entry.frontend.preview(entry.key, attachment.index)
                                ToolTip.visible: hovered
                                ToolTip.text: "Show this image (when OpenGhost is idle)"
                            }
                            RichImage {
                                id: shownPreview
                                objectName: "attachmentPreview"
                                anchors.right: parent.right
                                visible: attachment.preview === "ready"
                                kind: "image"
                                readonly property real fit: Math.min(1, 240 / Math.max(1, implicitHeight),
                                                                     entry.column * 0.8 / Math.max(1, implicitWidth))
                                width: implicitWidth * fit
                                height: implicitHeight * fit
                            }
                            // One of the message's selectable texts, after its
                            // card's; the window keeps it, so the selection is
                            // told (Selection.note).
                            SelectableLabel {
                                id: previewError
                                objectName: "previewError"
                                anchors.right: parent.right
                                readonly property string error: attachment.preview !== "ready"
                                                                && attachment.preview !== "loading"
                                                                ? attachment.preview : ""
                                visible: error.length > 0
                                width: Math.min(implicitWidth, entry.column * 0.8)
                                text: error
                                host: entry
                                unit: "u/f" + attachment.index + ":e"
                                onErrorChanged: Selection.note(entry.key, unit, error)
                                Component.onCompleted: Selection.note(entry.key, unit, error)
                                wrapMode: TextEdit.Wrap
                                color: Theme.danger
                                font.pixelSize: 12
                            }
                        }
                    }
                }
                // The bubble (.message-bubble): the prompt as sent, plain text.
                Rectangle {
                    anchors.right: parent.right
                    visible: entry.body.length > 0 || entry.attachments.length === 0
                    width: Math.min(user.room, user.natural) + 32
                    height: messageBody.implicitHeight + 20
                    radius: 20
                    color: Theme.accent
                    LiveText {
                        id: messageBody
                        objectName: "body"
                        x: 16
                        y: 10
                        width: parent.width - 32
                        padding: 0
                        content: entry.body
                        wrapMode: TextEdit.Wrap
                        color: Theme.onAccent
                        font.pixelSize: 16
                        lineHeight: 16 * 1.65
                        frontend: entry.frontend
                        // The conversation's selection, not its own.
                        selectByMouse: false
                        activeFocusOnPress: false
                        Component.onCompleted: Selection.enroll(messageBody, entry.key, "u:b")
                        // .message-bubble ::selection: rgba(bubble fg, .16), never held back.
                        background: SelectionWash {
                            objectName: "wash"
                            target: messageBody
                            range: { void Selection.revision; return Selection.range(entry.key, "u:b") }
                            color: Theme.alpha(Theme.onAccent, 0.16)
                        }
                    }
                }
                // A steering input's receipt, as OpenGhost reports it: a text of
                // the selection after the bubble's.
                Label {
                    id: receipt
                    objectName: "receipt"
                    readonly property string unit: "u:r"
                    Component.onCompleted: Selection.enroll(receipt, entry.key, unit)
                    SelectionWash {
                        z: -1
                        anchors.fill: parent
                        target: receipt
                        range: { void Selection.revision; return Selection.range(entry.key, "u:r") }
                        color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
                    }
                    anchors.right: parent.right
                    width: Math.min(implicitWidth, parent.width)
                    visible: text.length > 0
                    text: entry.preview
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.pixelSize: 13
                    color: entry.messageState === "refused" || entry.messageState === "unconfirmed"
                           || entry.messageState === "notApplied" ? Theme.danger : Theme.secondary
                }
            }
        }
    }

    Component {
        id: assistantMessage
        // The reply's blocks, and over them the pointer that selects across
        // all of them as one (SelectArea).
        Item {
            width: entry.column
            implicitHeight: reply.implicitHeight
            SelectArea {
                objectName: "selectArea"
                anchors.fill: parent
                z: 1
                host: entry
                content: reply
                view: entry.ListView.view
            }
            Column {
                id: reply
                width: entry.column
                spacing: 6
                // A long message parses off the GUI thread; until then its row
                // keeps roughly its height, so the view does not jump.
                Item {
                    visible: rich.empty && rich.renderable && !rich.revealing && entry.body.length > 0
                             && rich.rest.length === 0
                    width: parent.width
                    height: visible ? Math.min(20000, Math.ceil(entry.body.length / 90) * 26.4) : 0
                }
                Markdown {
                    objectName: "markdown"
                    visible: rich.renderable
                    width: parent.width
                    blocks: rich.blocks
                    host: entry
                    outer: true
                    // Wide tables and diagrams (.md-wide): up to 1320 px.
                    wideWidth: Math.min(entry.width - 56, 1320)
                    // A large message builds only the blocks near the view.
                    nearby: rich.lazy ? Qt.point(entry.nearby.x - y, entry.nearby.y - y)
                                      : Qt.point(-Infinity, Infinity)
                }
                // Past markdown's render bound (OpenGhost's too-large preview) or
                // its bound of formatted pieces, the text is plain; Copy still
                // takes it whole from OpenGhost. The note is one of the reply's
                // selectable texts, as 1.2's .message-note is.
                SelectableLabel {
                    objectName: "plainNote"
                    visible: text.length > 0
                    width: parent.width
                    text: rich.plainNote
                    host: entry
                    unit: "n:t"
                    wrapMode: TextEdit.Wrap
                    color: Theme.secondary
                    font.pixelSize: 13
                }
                Markdown {
                    objectName: "plain"
                    visible: !rich.renderable || rich.rest.length > 0
                    width: parent.width
                    blocks: rich.plain
                    host: entry
                    prefix: "p"
                    nearby: rich.lazy ? Qt.point(entry.nearby.x - y, entry.nearby.y - y)
                                      : Qt.point(-Infinity, Infinity)
                }
                Label {
                    objectName: "richNotice"
                    visible: rich.notice.length > 0
                    width: parent.width
                    text: rich.notice
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.secondary
                    font.pixelSize: 14
                }
                // After the run's answer, before its Copy (.reply-metrics).
                Metrics {
                    id: replyMetrics
                    visible: entry.metrics.length > 0
                    gap: 8 - parent.spacing
                    width: parent.width
                    template: entry.metrics
                    tip: entry.preview
                    started: entry.started
                    completed: entry.completed
                }
                // .message-tools: 10 px below, 6 px left of the text; 2 px
                // below the metrics (reply-metrics.css).
                Item {
                    visible: entry.copyable
                    readonly property real gap: (replyMetrics.visible ? 2 : 10) - parent.spacing
                    width: 28
                    height: 28 + gap
                    opacity: entry.toolsArrival
                    transform: Translate { y: -4 * (1 - entry.toolsArrival) }
                    MessageCopy {
                        x: -6
                        y: parent.gap
                    }
                }
            }
        }
    }

    Component {
        id: note
        // A transcript notice: one text of the conversation's selection.
        Item {
            width: entry.column
            implicitHeight: noteText.implicitHeight + (retry.visible ? retry.height + 8 : 0)
            Button {
                id: retry
                objectName: "retryTurn"
                z: 2
                y: noteText.implicitHeight + 8
                visible: entry.frontend && entry.frontend.retryRow === entry.key
                text: "Retry"
                onClicked: entry.frontend.retry()
                padding: 8
                horizontalPadding: 14
                background: Rectangle {
                    radius: 16
                    color: retry.hovered ? Theme.hover : Theme.composerBg
                    border.color: Theme.composerBorder
                }
                contentItem: Label { text: retry.text; color: Theme.text; font.pixelSize: 14 }
            }
            SelectArea {
                objectName: "selectArea"
                anchors.fill: parent
                z: 1
                host: entry
                content: noteText
                view: entry.ListView.view
            }
            LiveText {
                id: noteText
                objectName: "note"
                width: entry.column
                topPadding: 2
                bottomPadding: 2
                content: entry.body
                wrapMode: TextEdit.Wrap
                color: Theme.secondary
                font.pixelSize: 14
                frontend: entry.frontend
                selectByMouse: false
                activeFocusOnPress: false
                readonly property string unit: "n:t"
                Component.onCompleted: Selection.enroll(noteText, entry.key, unit)
                background: SelectionWash {
                    target: noteText
                    range: { void Selection.revision; return Selection.range(entry.key, "n:t") }
                    color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
                }
            }
        }
    }

}
