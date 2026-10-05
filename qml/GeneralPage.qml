import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Effects
import QtQuick.Shapes
import OpenGhost.Native

// Settings → General (OpenGhost's settings-general.js): standing instructions
// and files kept at hand for new chats. The disconnected preview is read-only;
// storage and application to future chats require a separate integration.
Column {
    id: page
    required property var store
    enabled: store.available
    // The status line's text and tone; it clears itself after 6 s.
    property string status
    property bool statusError: false
    // Files hovering over the Files section (.is-dropping).
    property bool dropping: false
    width: parent ? parent.width : 0

    function say(text, error) {
        status = text
        statusError = !!error
        statusTimer.restart()
    }
    // Chosen or dropped files; a refusal of the whole selection is said at once.
    function addFiles(urls) {
        say("")
        const refusal = store.add(urls)
        if (refusal)
            say(refusal, true)
    }
    // settings-general.js tokens(): ≈ chars / 3.2, in thousands past 999.
    function tokens(chars) {
        const count = Math.max(1, Math.round(chars / 3.2))
        return count < 1000 ? String(count)
            : (count / 1000).toFixed(count < 10000 ? 1 : 0).replace(/\.0$/, "") + "k"
    }
    // file-kinds.js formatSize: decimal units, one decimal below 10.
    function formatSize(bytes) {
        if (bytes < 1000)
            return bytes + " B"
        const units = ["KB", "MB", "GB"]
        let value = bytes / 1000, unit = 0
        while (value >= 1000 && unit < units.length - 1) {
            value /= 1000
            ++unit
        }
        return (value < 10 ? value.toFixed(1).replace(/\.0$/, "") : Math.round(value)) + " " + units[unit]
    }
    function grouped(n) { return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ",") }

    Timer {
        id: statusTimer
        interval: 6000
        onTriggered: page.status = ""
    }
    Connections {
        target: page.store
        function onAdded(error) {
            if (error)
                page.say(error, true)
        }
        // A change General could not save (NFE-02): said, never shown as done.
        function onSaveFailed(error) { page.say(error, true) }
        function onFilesChanged() { rows.sync() }
    }
    Component.onCompleted: rows.sync(true)

    // The rows shown: the store's files in order, and those folding away.
    ListModel {
        id: rows
        function sync(instant) {
            const files = page.store.files
            const keep = new Set(files.map(f => f.id))
            for (let i = 0; i < count; ++i) {
                if (!keep.has(get(i).id) && !get(i).leaving)
                    setProperty(i, "leaving", true)
            }
            // New files open in place; the rest stay put.
            let at = 0
            for (const file of files) {
                while (at < count && get(at).leaving)
                    ++at
                if (at < count && get(at).id === file.id) {
                    set(at, { file: file })
                    ++at
                    continue
                }
                let found = -1
                for (let i = at; i < count; ++i) {
                    if (get(i).id === file.id) {
                        found = i
                        break
                    }
                }
                if (found >= 0) {
                    move(found, at, 1)
                    set(at, { file: file })
                } else {
                    insert(at, { id: file.id, file: file, leaving: false, instant: !!instant })
                }
                ++at
            }
        }
        function drop(id) {
            for (let i = 0; i < count; ++i) {
                if (get(i).id === id && get(i).leaving) {
                    remove(i)
                    return
                }
            }
        }
    }

    // .settings-lead: 13.5/20, 4 px below, which the block's padding adds to.
    PageText {
        objectName: "generalLead"
        width: parent.width
        text: "General settings are not connected yet. Nothing here is saved or sent."
        size: 13.5
        line: 20
        color: Theme.secondary
        wrapMode: Text.Wrap
    }
    Item { width: 1; height: 4 }

    // .general-block: 20 px above, 18 below.
    Column {
        width: parent.width
        topPadding: 20
        bottomPadding: 18
        // .general-head: the label, and on the right the count near the limit.
        Item {
            width: parent.width
            height: instructionsLabel.height
            PageText {
                id: instructionsLabel
                text: "Instructions"
                size: 15
                cssWeight: 500
                color: Theme.text
            }
            PageText {
                objectName: "generalCount"
                readonly property int length: field.length
                anchors.right: parent.right
                anchors.baseline: instructionsLabel.baseline
                size: 12
                font.features: { "tnum": 1 }
                color: Theme.secondary
                text: length > page.store.maxInstructions * 0.85
                      ? page.grouped(length) + " / " + page.grouped(page.store.maxInstructions) : ""
            }
        }
        PageText {
            width: parent.width
            topPadding: 4 + Theme.halfLeading(font, line)
            text: "How to answer and what to know about you. Every new chat gets them, and a long chat keeps them after it is compacted."
            size: 13
            line: 18
            color: Theme.secondary
            wrapMode: Text.Wrap
        }
        Item { width: 1; height: 12 }
        // .general-field: the composer's colour and inset line, radius 16;
        // .2 when focused. Its height follows the text on SmoothHeight's
        // spring (260/32), from 116 px to 320, then the text scrolls.
        Item {
            id: fieldBox
            objectName: "generalField"
            readonly property real goal: Math.max(116, Math.min(320, field.contentHeight + 26))
            width: parent.width
            height: Math.round(grow.value)
            Spring {
                id: grow
                k: 260
                c: 32
                within: 0.1
                slower: 0.1
                goal: fieldBox.goal
                Component.onCompleted: snap()
            }
            Rectangle {
                anchors.fill: parent
                radius: 16
                antialiasing: true
                color: field.activeFocus ? Theme.alpha(Theme.fg, 0.2) : Theme.composerBorder
                Behavior on color { enabled: !Theme.reducedMotion; ColorAnimation { duration: 200 } }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: 15
                    antialiasing: true
                    color: Theme.composerBg
                }
            }
            Flickable {
                id: fieldScroll
                anchors.fill: parent
                clip: true
                contentWidth: width
                contentHeight: field.contentHeight + 26
                boundsBehavior: Flickable.StopAtBounds
                function reveal(r) {
                    if (contentY > r.y)
                        contentY = r.y
                    else if (contentY + height < r.y + 26 + r.height)
                        contentY = r.y + 26 + r.height - height
                }
                TextEdit {
                    id: field
                    objectName: "generalInstructions"
                    x: 16
                    y: 13
                    width: fieldScroll.width - 32
                    // At least the four rows' box the field keeps.
                    height: Math.max(contentHeight, 116 - 26)
                    font.pointSize: Theme.points(14)
                    color: Theme.text
                    selectionColor: Theme.alpha(Theme.selection, 0.3 / Theme.selection.a)
                    selectedTextColor: Theme.text
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    persistentSelection: true
                    activeFocusOnTab: true
                    Accessible.role: Accessible.EditableText
                    Accessible.name: "Instructions"
                    onCursorRectangleChanged: if (activeFocus) fieldScroll.reveal(cursorRectangle)
                    Component.onCompleted: {
                        text = page.store.instructions
                        relines()
                    }
                    function relines() { Theme.fieldLines(textDocument, 21) }
                    // maxlength: text past the limit is cut as it arrives.
                    onTextChanged: {
                        if (length > page.store.maxInstructions)
                            remove(page.store.maxInstructions, length)
                        page.store.instructions = text
                        relines()
                        Qt.callLater(relines)
                    }
                    Connections {
                        target: page.store
                        function onInstructionsChanged() {
                            if (field.text !== page.store.instructions)
                                field.text = page.store.instructions
                        }
                    }
                    PageText {
                        width: parent.width
                        visible: field.length === 0 && field.preeditText.length === 0
                        text: "For example: answer briefly and to the point, explain code step by step, use metric units."
                        size: 14
                        line: 21
                        color: Theme.muted
                        wrapMode: Text.Wrap
                    }
                }
            }
            // scrollbar-width: thin in rgba(fg, .2), as Chromium draws it: a
            // 6 px thumb 2 px from the edge between two small arrows.
            Item {
                objectName: "generalFieldScrollbar"
                readonly property real room: height - 25
                x: parent.width - 11
                width: 11
                height: parent.height
                visible: fieldScroll.contentHeight > fieldScroll.height + 1
                Rectangle {
                    x: 3
                    y: 12.5 + parent.room * fieldScroll.contentY / fieldScroll.contentHeight
                    width: 6
                    height: Math.max(12, parent.room * fieldScroll.height / fieldScroll.contentHeight)
                    radius: 3
                    color: Theme.alpha(Theme.fg, 0.2)
                }
                Shape {
                    anchors.fill: parent
                    layer.enabled: true
                    layer.samples: 4
                    ShapePath {
                        strokeColor: "transparent"
                        fillColor: Theme.alpha(Theme.fg, 0.2)
                        PathMove { x: 5.5; y: 4 }
                        PathLine { x: 8.5; y: 7.5 }
                        PathLine { x: 2.5; y: 7.5 }
                        PathLine { x: 5.5; y: 4 }
                        PathMove { x: 5.5; y: fieldBox.height - 5.5 }
                        PathLine { x: 2.5; y: fieldBox.height - 9 }
                        PathLine { x: 8.5; y: fieldBox.height - 9 }
                        PathLine { x: 5.5; y: fieldBox.height - 5.5 }
                    }
                }
            }
        }
    }

    // The Files block, under a .06 line; files dropped anywhere on it are added.
    Item {
        objectName: "generalFiles"
        width: parent.width
        height: filesBlock.height
        Rectangle {
            width: parent.width
            height: 1
            color: Theme.alpha(Theme.fg, 0.06)
        }
        DropArea {
            anchors.fill: parent
            onEntered: drag => {
                page.dropping = drag.hasUrls
                if (drag.hasUrls)
                    drag.accept(Qt.CopyAction)
            }
            onExited: page.dropping = false
            onDropped: drop => {
                page.dropping = false
                if (!drop.hasUrls)
                    return
                drop.accept(Qt.CopyAction)
                page.addFiles(drop.urls)
            }
        }
        Column {
            id: filesBlock
            width: parent.width
            topPadding: 21
            bottomPadding: 18
            PageText {
                text: "Files"
                size: 15
                cssWeight: 500
                color: Theme.text
            }
            PageText {
                width: parent.width
                topPadding: 4 + Theme.halfLeading(font, line)
                text: "Notes, a style guide, a CV: whatever OpenGhost should always have at hand. Each new chat starts with them attached."
                size: 13
                line: 18
                color: Theme.secondary
                wrapMode: Text.Wrap
            }
            // .general-files: 14 px below the hint while it holds any row.
            Item { width: 1; height: rows.count ? 14 : 0 }
            Column {
                width: parent.width
                Repeater {
                    model: rows
                    delegate: FileRow {}
                }
            }
            Item { width: 1; height: 12 }
            // .general-drop: one wide dashed target, pressed to choose files or
            // dropped onto; while files hover over the section it lights up.
            AbstractButton {
                id: dropButton
                objectName: "generalDrop"
                readonly property bool lit: hovered || visualFocus || page.dropping
                width: parent.width
                height: 52
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                text: "Drop files here or choose them"
                Accessible.name: text
                scale: page.dropping ? 1.015 : down ? 0.99 : 1
                Behavior on scale { enabled: !Theme.reducedMotion; NumberAnimation { duration: 350; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
                onClicked: picker.choose()
                background: Item {
                    Rectangle {
                        anchors.fill: parent
                        radius: 14
                        color: Theme.alpha(Theme.fg, page.dropping ? 0.05 : 0)
                        Behavior on color { enabled: !Theme.reducedMotion; ColorAnimation { duration: 200 } }
                    }
                    // border: 1.5px dashed, .16 at rest, .3 lit, .5 while
                    // dropping. Chromium floors border widths to device
                    // pixels: one pixel, in 3 px dashes 2 px apart.
                    Shape {
                        anchors.fill: parent
                        // Smooth corners; the straight runs stay on whole pixels.
                        layer.enabled: true
                        layer.samples: 4
                        ShapePath {
                            property real tone: page.dropping ? 0.5 : dropButton.lit ? 0.3 : 0.16
                            Behavior on tone { enabled: !Theme.reducedMotion; NumberAnimation { duration: 200 } }
                            strokeColor: Theme.alpha(Theme.fg, tone)
                            strokeWidth: 1
                            strokeStyle: ShapePath.DashLine
                            dashPattern: [3, 2]
                            capStyle: ShapePath.FlatCap
                            fillColor: "transparent"
                            PathRectangle {
                                x: 0.5
                                y: 0.5
                                width: dropButton.width - 1
                                height: dropButton.height - 1
                                radius: 13.5
                            }
                        }
                    }
                }
                contentItem: Item {
                    Row {
                        anchors.centerIn: parent
                        spacing: 8
                        PathIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 15
                            height: 15
                            name: "plus-thin"
                            color: dropButton.lit ? Theme.text : Theme.secondary
                            rotation: page.dropping ? 90 : 0
                            scale: page.dropping ? 1.15 : 1
                            Behavior on rotation { enabled: !Theme.reducedMotion; NumberAnimation { duration: 450; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
                            Behavior on scale { enabled: !Theme.reducedMotion; NumberAnimation { duration: 450; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
                            Behavior on color { enabled: !Theme.reducedMotion; ColorAnimation { duration: 200 } }
                        }
                        PageText {
                            anchors.verticalCenter: parent.verticalCenter
                            text: dropButton.text
                            size: 13.5
                            cssWeight: 500
                            color: dropButton.lit ? Theme.text : Theme.secondary
                            Behavior on color { enabled: !Theme.reducedMotion; ColorAnimation { duration: 200 } }
                        }
                    }
                }
            }
            // .settings-status.general-status: at least a line, 6 px below, left.
            // A store an older build saved with more pictures than one message
            // carries says so while nothing newer is said.
            PageText {
                objectName: "generalStatus"
                readonly property int crowded: page.store.pictures - page.store.maxPictures
                readonly property string standing: crowded > 0
                    ? "New chats can't start with " + page.store.pictures + " pictures; one "
                      + "message carries at most " + page.store.maxPictures + ". Remove "
                      + crowded + (crowded === 1 ? " picture." : " pictures.")
                    : ""
                width: parent.width
                topPadding: 6 + Theme.halfLeading(font, line)
                height: 6 + Math.max(1, lineCount) * line
                text: page.status || standing
                size: 13
                line: 18
                wrapMode: Text.Wrap
                color: page.statusError || !page.status && standing ? Theme.danger : Theme.secondary
                Accessible.role: Accessible.StaticText
            }
        }
    }

    // 1.2 gives a file's name its path as a title, which Chromium shows in
    // its own tooltip.
    TitleTip {
        id: tip
        objectName: "generalFileTip"
        parent: page.Overlay.overlay
        active: page.visible
    }

    // The chooser exists only while open; its URLs go straight to the store,
    // which keeps paths off QML.
    Loader {
        id: picker
        objectName: "generalPicker"
        active: false
        sourceComponent: FileDialog {
            title: "Keep files for every new chat"
            fileMode: FileDialog.OpenFiles
            onAccepted: {
                page.addFiles(selectedFiles)
                picker.finish()
            }
            onRejected: picker.finish()
        }
        function choose() {
            active = true
            item.open()
        }
        function finish() { Qt.callLater(() => { picker.active = false }) }
    }

    // CSS text in a line box of `line` px, its glyphs half the leading down.
    component PageText: Text {
        property real size: 15
        property real line: size * 1.5
        property int cssWeight: 400
        font.pointSize: Theme.points(size)
        font.weight: Theme.weight(cssWeight)
        lineHeightMode: Text.FixedHeight
        lineHeight: line
        topPadding: Theme.halfLeading(font, line)
        bottomPadding: -Theme.halfLeading(font, line)
        height: topPadding - Theme.halfLeading(font, line) + Math.max(1, lineCount) * line
        textFormat: Text.PlainText
    }

    // .general-file: 54 px on the composer's colour, radius 14; the kind's
    // icon or the picture, the name and "Kind · size · ≈tokens", and the ✕
    // that shows under the pointer or keyboard focus. A new row opens
    // (0.42 s) and a removed one folds away (0.32 s), blurring as it goes.
    component FileRow: Item {
        id: row
        required property var model
        required property int index
        readonly property var file: model.file
        readonly property bool leaving: model.leaving
        property real fold: model.instant || Theme.reducedMotion ? 1 : 0
        objectName: "generalFile"
        width: parent ? parent.width : 0
        // The list's 6 px gap above every row but the first; a leaving row's
        // gap closes with it (margin-top: −6px).
        readonly property real gap: index > 0 ? (leaving ? 6 * fold : 6) : 0
        height: gap + 54 * fold
        enabled: !leaving
        Component.onCompleted: if (fold < 1) opening.start()
        onLeavingChanged: {
            if (!leaving)
                return
            opening.stop()
            if (Theme.reducedMotion)
                rows.drop(file.id)
            else
                closing.start()
        }
        NumberAnimation {
            id: opening
            target: row
            property: "fold"
            to: 1
            duration: 420
            easing.type: Easing.Bezier
            easing.bezierCurve: Theme.motion
        }
        NumberAnimation {
            id: closing
            target: row
            property: "fold"
            to: 0
            duration: 320
            easing.type: Easing.Bezier
            easing.bezierCurve: Theme.motion
            onFinished: rows.drop(row.file.id)
        }
        HoverHandler { id: rowHover }
        Item {
            id: card
            y: row.gap
            width: parent.width
            height: 54 * row.fold
            clip: true
            opacity: row.fold
            layer.enabled: row.fold < 1
            layer.effect: MultiEffect {
                blurEnabled: true
                blurMax: 16
                autoPaddingEnabled: true
                // blur(4px) folded.
                blur: Math.min(1, 4 * (1 - row.fold) / (0.27 * 16))
            }
            Rectangle {
                anchors.fill: parent
                radius: 14
                antialiasing: true
                color: Theme.composerBorder
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: 13
                    antialiasing: true
                    color: Theme.composerBg
                }
            }
            // Centred in the row as it opens (align-items: center), and
            // coming down 6 px as it opens.
            Item {
                width: parent.width
                height: 54
                y: (card.height - 54) / 2 - (row.leaving ? 0 : 6 * (1 - row.fold))
                FileIcon {
                    id: kind
                    x: 12 + 3.5
                    y: (54 - 31) / 2
                    width: 25
                    height: 31
                    visible: !thumb.shown
                    fileName: row.file.name
                }
                // Icons and pictures take the same 32 px, so the names line up.
                Item {
                    id: thumb
                    property bool shown: false
                    function refresh() {
                        shown = row.file.kind === "image" && page.store.hasThumbnail(row.file.id)
                        tile.image = page.store.thumbnail(row.file.id)
                    }
                    Component.onCompleted: refresh()
                    Connections {
                        target: page.store
                        function onThumbnailsChanged() { thumb.refresh() }
                    }
                    x: 12
                    y: 11
                    width: 32
                    height: 32
                    visible: shown
                    ImageTile {
                        id: tile
                        objectName: "generalFileThumb"
                        anchors.fill: parent
                        radius: 8
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: "transparent"
                        border.width: 1
                        border.color: Theme.alpha(Theme.fg, 0.08)
                    }
                }
                // Centred as Chromium places its half pixel: down.
                Column {
                    x: 12 + 32 + 12
                    y: Math.ceil((54 - height) / 2)
                    width: parent.width - x - 12 - 30 - 9
                    spacing: 1
                    PageText {
                        id: name
                        objectName: "generalFileName"
                        width: parent.width
                        text: row.file.name
                        size: 14
                        line: 19
                        cssWeight: 600
                        color: Theme.text
                        elide: Text.ElideRight
                        // Its path in Chromium's tooltip (1.2's title).
                        HoverHandler {
                            readonly property string file: row.file.id
                            onHoveredChanged: hovered ? tip.hover(name, () => page.store.tip(file), point.position)
                                                      : tip.leave(name)
                            onPointChanged: if (hovered) tip.hover(name, () => page.store.tip(file), point.position)
                        }
                    }
                    PageText {
                        objectName: "generalFileMeta"
                        width: parent.width
                        text: {
                            const f = row.file
                            const parts = [kind.kind, page.formatSize(f.size)]
                            if (f.kind === "text")
                                parts.push("≈" + page.tokens(f.chars) + " tokens")
                            else if (f.kind === "image" && f.width)
                                parts.push(f.width + "×" + f.height)
                            return parts.join(" · ")
                        }
                        size: 12.5
                        line: 17
                        color: Theme.secondary
                        elide: Text.ElideRight
                    }
                }
                AbstractButton {
                    id: removeButton
                    objectName: "generalFileRemove"
                    readonly property bool lit: hovered || visualFocus
                    x: parent.width - 9 - 30
                    y: 12
                    width: 30
                    height: 30
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.name: "Remove " + row.file.name
                    opacity: rowHover.hovered || visualFocus ? 1 : 0
                    Behavior on opacity { enabled: !Theme.reducedMotion; NumberAnimation { duration: 200 } }
                    onClicked: {
                        page.say("")
                        page.store.remove(row.file.id)
                    }
                    background: Rectangle {
                        radius: 15
                        color: Theme.alpha(Theme.fg, removeButton.lit ? 0.08 : 0)
                        Behavior on color { enabled: !Theme.reducedMotion; ColorAnimation { duration: 200 } }
                    }
                    contentItem: Item {
                        PathIcon {
                            anchors.centerIn: parent
                            width: 12
                            height: 12
                            name: "close"
                            color: removeButton.lit ? Theme.text : Theme.secondary
                            Behavior on color { enabled: !Theme.reducedMotion; ColorAnimation { duration: 200 } }
                        }
                    }
                }
            }
        }
    }
}
