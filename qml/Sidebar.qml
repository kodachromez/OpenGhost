import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Shapes
import OpenGhost.Native

// OpenGhost's saved conversations by folder, newest first, drawn as its
// sidebar (index.html .sidebar, chat-list.js, search-field.js). OpenGhost
// 1.3's chats without a folder lead the list in their own group ("Chats",
// keyed "home:"), then the "Folders" heading with New Folder and each
// folder. New Chat is a local draft until its first send, shown first in the
// group of the folder the window is attached to; switching waits while a run
// is active.
Pane {
    id: sidebar
    required property var frontend
    signal done() // Search finished: focus returns to the composer.
    signal settingsRequested()
    // An OpenGhost action unavailable in this disconnected port; the text
    // says why and that nothing changed.
    signal refused(string text)
    signal folderWanted() // New Folder: the folder chooser (Main.qml).
    padding: 0
    font.pixelSize: 14 // --sidebar-font-size
    // It lies on the window's backdrop (Main.qml), not a fill of its own.
    background: null

    readonly property var sessions: frontend.sessions
    readonly property bool searching: search.text.length > 0
    readonly property bool drafting: frontend.session === "" && !searching
    // The group New Chat's draft waits in: "home:" when the window is
    // attached to the chats without a folder, else the listed or kept empty
    // folder it is attached to, if any.
    readonly property string home: {
        frontend.switching // Attaching to another folder changes it.
        if (frontend.isWorkspace(""))
            return "home:"
        return sessions.folders.find(folder => frontend.isWorkspace(folder))
               || sessions.emptyFolders.find(folder => frontend.isWorkspace(folder)) || ""
    }
    // A group's key: a folder's path, or "home:" for the chats without one.
    function groupKey(folder) { return folder === "" ? "home:" : folder }
    // The home group shows while it has chats or the list is not searched
    // (OpenGhost keeps it, empty, for its New chat and the draft).
    readonly property bool homeShown: sessions.homeCount > 0 || !searching
    // It shows with no chat listed in it (its own group stands elsewhere).
    readonly property bool emptyHome: sessions.homeCount === 0 && !searching
    // The Folders heading keeps 12 px from anything above it.
    readonly property bool headingGap: sessions.pinnedCount > 0 || homeShown
    // Folder paths collapsed by the reader: memory only, as in OpenGhost.
    // A search, and the draft's own folder, show a folder open.
    property var collapsed: ({})
    function isCollapsed(folder) {
        const key = groupKey(folder)
        return !searching && collapsed[key] === true
               && !(drafting && key === home)
    }
    function toggleFolder(folder) {
        folder = groupKey(folder)
        const next = Object.assign({}, collapsed)
        if (next[folder])
            delete next[folder]
        else
            next[folder] = true
        collapsed = next
    }
    function hiddenAt(row) {
        return row >= sessions.pinnedCount && isCollapsed(sessions.folderAt(row))
    }
    // Each folder's collapse as it shows (chat-list.js collapse()): the body
    // closes over its rows in 0.48 s, they fade in 0.3 s, and the folder's
    // front turns in 0.45 s; at once with reduced motion.
    Instantiator {
        id: foldings
        model: ["home:"].concat(sidebar.sessions.folders)
        delegate: QtObject {
            required property string modelData
            readonly property bool shut: sidebar.isCollapsed(modelData)
            property real fold: shut ? 1 : 0
            property real shade: shut ? 0 : 1
            property real front: shut ? 1 : 0
            // Delete Folder confirmed: the folder's rows fold away with it.
            property real gone: sidebar.leaving[modelData] === true ? 1 : 0
            Behavior on gone {
                enabled: !Theme.reducedMotion
                NumberAnimation { duration: 320; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
            Behavior on fold {
                enabled: !Theme.reducedMotion
                NumberAnimation { duration: 480; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
            Behavior on shade {
                enabled: !Theme.reducedMotion
                NumberAnimation { duration: 300; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
            }
            Behavior on front {
                enabled: !Theme.reducedMotion
                NumberAnimation { duration: 450; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
        }
    }
    function folding(folder) {
        folder = groupKey(folder)
        for (let i = 0; i < foldings.count; ++i) {
            const found = foldings.objectAt(i)
            if (found && found.modelData === folder)
                return found
        }
        return null
    }
    // A listed row's place in its folder's run of rows, and the run's length.
    property int revision: 0
    function run(row) {
        revision // The rows moved.
        const folder = sessions.folderAt(row)
        let first = row, last = row
        while (first - 1 >= sessions.pinnedCount && sessions.folderAt(first - 1) === folder)
            --first
        while (last + 1 < chats.count && sessions.folderAt(last + 1) === folder)
            ++last
        return { place: row - first, length: last - first + 1 }
    }
    function name(folder) {
        if (folder === "" || folder === "home:")
            return "Chats"
        return folder.split("/").filter(part => part.length).pop() || folder
    }

    // Relative times (chat-list.js ago()), refreshed every 30 s and with
    // each list change.
    property double now: Date.now()
    function ago(time, now) {
        const d = Math.max(0, now - time), minute = 60000, hour = 60 * minute,
              day = 24 * hour, week = 7 * day
        if (d < minute)
            return "now"
        if (d < hour)
            return Math.floor(d / minute) + "m"
        if (d < day)
            return Math.floor(d / hour) + "h"
        if (d < week)
            return Math.floor(d / day) + "d"
        if (d < 5 * week)
            return Math.floor(d / week) + "w"
        return Qt.locale("en_US").toString(new Date(time), "MMM d")
    }
    Timer {
        interval: 30000
        running: true
        repeat: true
        onTriggered: sidebar.now = Date.now()
    }
    Connections {
        target: sidebar.sessions
        function onModelReset() { sidebar.now = Date.now(); ++sidebar.revision }
        function onRowsInserted() { sidebar.now = Date.now(); ++sidebar.revision }
        function onRowsRemoved() { ++sidebar.revision }
        function onRowsMoved() { sidebar.now = Date.now(); ++sidebar.revision }
        function onDataChanged() { sidebar.now = Date.now() }
    }

    // Ctrl+K opens the field even while it is folded away.
    property bool opening: false
    function focusSearch() {
        opening = true
        search.forceActiveFocus()
        search.selectAll()
    }
    // Escape from the field or its results: the search ends, focus returns.
    function endSearch() {
        search.text = ""
        sidebar.done()
    }
    // New chat (the header's, OpenGhost 1.3's sidebar-new-chat): a chat
    // without a folder.
    function newChat() {
        newChatIn("")
    }
    // A group's New chat: ask OpenGhost to select the requested folder.
    // The disconnected facade refuses folders; no service is launched.
    function newChatIn(folder) {
        if (!frontend.canSwitch)
            return
        const refusal = frontend.newChatIn(folder === "home:" ? "" : folder)
        if (refusal)
            sidebar.refused(refusal)
        else
            sidebar.done()
    }

    // The hover glide (.chats-glide): one fill that springs to the hovered
    // row or folder head and fades out when the pointer leaves the list.
    property Item hoverItem: null
    function hover(item, on) {
        if (on)
            hoverItem = item
        else if (hoverItem === item)
            hoverItem = null
    }
    function track() {
        if (!hoverItem)
            return
        glideY.goal = hoverItem.mapToItem(chats.contentItem, 0, 0).y
        glideH.goal = hoverItem.height
        if (glideO.value < 0.02) {
            glideY.snap()
            glideH.snap()
        }
    }
    onHoverItemChanged: track()
    Spring { id: glideY; k: 520; c: 40; within: 0.01; slower: 0.05 }
    Spring { id: glideH; k: 520; c: 40; within: 0.01; slower: 0.05 }
    Spring { id: glideO; goal: sidebar.hoverItem ? 1 : 0; k: 320; c: 32; within: 0.01; slower: 0.05 }
    // Renaming happens in the row (chat-list.js rename()): one chat at a time.
    // OpenGhost renames it; the row shows the title OpenGhost answers with.
    property string renamingId: ""
    property Item renameField: null // The open field, for presses outside it.
    function startRename(sessionId, index) {
        if (renamingId !== "")
            return
        // The current row keeps its delegate while the list scrolls; it
        // takes the list's focus first, then the field takes it.
        chats.currentIndex = index
        renamingId = sessionId
    }
    function finishRename(sessionId, keep, text, original) {
        if (renamingId !== sessionId)
            return
        renamingId = ""
        // Whitespace runs as one space, trimmed; an unchanged name is not sent.
        const title = text.replace(/\s+/g, " ").trim()
        if (keep && title && title !== original)
            frontend.rename(sessionId, title)
    }
    // Deleting (chat-list.js askDelete(), keep(), remove()): Delete asks and
    // a second Delete within 3 s deletes; the pointer leaving the row, or the
    // wait running out, keeps the chat. OpenGhost deletes it: the row takes no
    // more clicks until OpenGhost answers, leaves only once OpenGhost confirms, and stays
    // after a refusal, whose reason is shown as a notice.
    property string confirmingId: ""
    property var deleting: ({}) // Session IDs whose delete OpenGhost has not refused.
    function askDelete(sessionId) {
        if (!sessionId || deleting[sessionId])
            return
        if (confirmingId !== sessionId) {
            confirmingId = sessionId
            confirmWait.restart()
            return
        }
        confirmWait.stop()
        confirmingId = ""
        const next = Object.assign({}, deleting)
        next[sessionId] = true
        deleting = next
        frontend.remove(sessionId)
    }
    function keepChat(sessionId) {
        if (confirmingId !== sessionId)
            return
        confirmWait.stop()
        confirmingId = ""
    }
    Timer { id: confirmWait; interval: 3000; onTriggered: sidebar.confirmingId = "" }
    // Delete Folder (chat-list.js askDeleteFolder(), keepFolder(),
    // removeFolder()) asks the same way. Confirmed, the folder and its rows
    // fold away (REMOVE) and OpenGhost then deletes each of its chats; the folder
    // is forgotten once all are gone, and comes back if OpenGhost keeps any.
    property string confirmingFolder: ""
    property var leaving: ({}) // Folders confirmed, until gone or kept.
    property var folderQueue: [] // Confirmed folders still folding away.
    function askDeleteFolder(folder) {
        if (!folder || leaving[folder])
            return
        if (confirmingFolder !== folder) {
            confirmingFolder = folder
            folderConfirmWait.restart()
            return
        }
        folderConfirmWait.stop()
        confirmingFolder = ""
        const next = Object.assign({}, leaving)
        next[folder] = true
        leaving = next
        folderQueue = folderQueue.concat([folder])
        if (Theme.reducedMotion)
            removeFolders()
        else
            folderLeave.restart()
    }
    function keepFolder(folder) {
        if (confirmingFolder !== folder)
            return
        folderConfirmWait.stop()
        confirmingFolder = ""
    }
    function removeFolders() {
        const queued = folderQueue
        folderQueue = []
        for (const folder of queued)
            frontend.removeFolder(folder)
    }
    function folderLabel(folder) {
        const count = frontend.folderChats(folder)
        return count ? "Click again to delete the folder and its chats (" + count
                       + "). Files on disk stay"
                     : "Click again to remove the folder from the list. Files on disk stay"
    }
    Timer { id: folderConfirmWait; interval: 3000; onTriggered: sidebar.confirmingFolder = "" }
    Timer { id: folderLeave; interval: 320; onTriggered: sidebar.removeFolders() }
    Connections {
        target: sidebar.sessions
        // A folder that left is forgotten here too, so one added again shows.
        function onGroupsChanged() {
            const present = sidebar.sessions.arranged.concat(sidebar.sessions.emptyFolders)
            const next = Object.assign({}, sidebar.leaving)
            let changed = false
            for (const folder in next) {
                if (!present.includes(folder) && !sidebar.folderQueue.includes(folder)) {
                    delete next[folder]
                    changed = true
                }
            }
            if (changed)
                sidebar.leaving = next
        }
    }
    Connections {
        target: sidebar.frontend
        // A deleted chat's row leaves with the next list and its ID is never
        // listed again; a kept one takes clicks again.
        function onSessionRemoved(sessionId, deleted) {
            if (deleted)
                return
            const next = Object.assign({}, sidebar.deleting)
            delete next[sessionId]
            sidebar.deleting = next
        }
        // A folder OpenGhost kept chats in comes back; why was said already.
        function onFolderRemoved(folder, removed, notice) {
            if (removed)
                return
            const next = Object.assign({}, sidebar.leaving)
            delete next[folder]
            sidebar.leaving = next
            if (notice)
                sidebar.refused(notice)
        }
    }
    // A press anywhere else keeps the name (a capturing pointerdown).
    Item {
        parent: sidebar.Window.contentItem
        anchors.fill: parent
        z: 1e6
        enabled: sidebar.renameField !== null
        PointHandler {
            onActiveChanged: {
                const field = sidebar.renameField
                if (active && field && !field.contains(field.mapFromItem(null, point.scenePosition)))
                    field.finish(true, false)
            }
        }
    }

    // What lies under a row's title end, for its fade: the active fill, or
    // this row's share of the glide (--row-hover-bg) over the backdrop.
    function under(item, active) {
        const glide = hoverItem === item ? Math.min(1, Math.max(0, glideO.value)) : 0
        return active ? Theme.rowActive
                      : Theme.alpha(Theme.rowHover, glide)
    }

    // `value` follows `when` after `after` ms when it turns on and at once
    // when it turns off: a CSS transition-delay on the lit rule only.
    component Later: QtObject {
        id: later
        property bool when: false
        property int after: 0
        property bool value: false
        onWhenChanged: {
            wait.stop()
            if (when && after > 0 && !Theme.reducedMotion)
                wait.start()
            else
                value = when
        }
        property Timer wait: Timer { interval: later.after; onTriggered: later.value = later.when }
    }
    // An action's trash, its lid lifting and tipping back while Delete waits
    // for its second click (.is-confirming .trash-lid: translate(-1, -3)
    // rotate(-14deg) about (40, 45), 0.4 s with a small overshoot).
    component TrashGlyph: Item {
        id: trash
        required property Action action
        property bool lifted: false
        property real lift: lifted ? 1 : 0
        Behavior on lift {
            enabled: !Theme.reducedMotion
            NumberAnimation {
                duration: 400
                easing.type: Easing.Bezier
                easing.bezierCurve: [0.34, 1.56, 0.64, 1, 1, 1]
            }
        }
        PathIcon {
            objectName: "trashLid"
            x: Math.ceil((trash.action.width - width) / 2)
            y: Math.ceil((trash.action.height - height) / 2)
            width: trash.action.iconSize
            height: trash.action.iconSize
            name: "trash-lid"
            color: trash.action.tint
            transform: [
                Rotation { origin.x: 10 * 15 / 60; origin.y: 15 * 15 / 60; angle: -14 * trash.lift },
                Translate { x: -1 * 15 / 60 * trash.lift; y: -3 * 15 / 60 * trash.lift }
            ]
        }
        PathIcon {
            objectName: "trashBody"
            x: Math.ceil((trash.action.width - width) / 2)
            y: Math.ceil((trash.action.height - height) / 2)
            width: trash.action.iconSize
            height: trash.action.iconSize
            name: "trash-body"
            color: trash.action.tint
        }
    }
    // A 26 px action (.chat-action, .chats-folder-action, .chats-heading-action).
    component Action: AbstractButton {
        id: action
        property string glyph
        property real iconSize: 15
        property real rest: 0.5
        property real lit: 0.92
        property real wash: 0.08
        property bool on: false
        // Asking to be clicked again to delete: danger at .16 under the
        // glyph in danger, over hover (.chat-row.is-confirming .is-delete).
        property bool danger: false
        property bool drawn: true // False: the action draws its own glyph in `tint`.
        readonly property alias tint: stroke.color
        width: 26
        height: 26
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        background: Rectangle {
            radius: 7
            color: action.danger ? Theme.alpha(Theme.danger, 0.16)
                                 : Theme.alpha(Theme.strong, action.hovered ? action.wash : 0)
            Behavior on color { ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
        }
        contentItem: Item {
            // Centred in whole pixels, rounded up as Chromium places them.
            PathIcon {
                x: Math.ceil((action.width - width) / 2)
                y: Math.ceil((action.height - height) / 2)
                width: action.iconSize
                height: action.iconSize
                name: action.glyph + "-fill"
                visible: action.on && action.drawn
                color: stroke.color
            }
            PathIcon {
                id: stroke
                x: Math.ceil((action.width - width) / 2)
                y: Math.ceil((action.height - height) / 2)
                width: action.iconSize
                height: action.iconSize
                name: action.glyph
                visible: action.drawn
                color: action.danger ? Theme.danger
                                     : Theme.alpha(Theme.strong, action.hovered ? action.lit
                                                   : action.on ? 0.85 : action.rest)
                Behavior on color { ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
            }
        }
    }
    // A section heading (.chats-heading): 30 px, 12.5 px/500 at .55, with
    // New Folder at its end.
    component Heading: Item {
        id: heading
        property alias text: label.text
        property bool withAction: true
        signal act()
        height: 30
        Text {
            id: label
            x: 20
            // CSS centres a 1.5 line box; the text sits half its leading
            // lower, less the pixel Chromium's whole-pixel ascent and
            // descent take off at this size (measured).
            y: (heading.height - 18.75) / 2 + Theme.halfLeading(font, 18.75) - 1
            font.pointSize: Theme.points(12.5)
            font.weight: Theme.weight(500)
            color: Theme.secondary
        }
        Action {
            objectName: "newFolder"
            visible: heading.withAction
            x: heading.width - 10 - 2 - width
            y: 2
            glyph: "folder-add"
            iconSize: 16
            rest: 0.55
            lit: 0.9
            wash: 0.07
            Accessible.name: "New Folder"
            ButtonTip { text: "New Folder" }
            onClicked: heading.act()
        }
    }
    // A chat row's face (.chat-row): 32 px, radius 9, a 5 px dot, the title
    // fading out at its end, and the time or, when lit, rename, pin and
    // delete. Renaming turns the row into a field (.chat-row.is-renaming).
    component RowFace: Rectangle {
        id: face
        property string title
        property string key // The chat shown: a new title for it writes itself in.
        property bool pooled: false // The delegate waits in the pool to be reused.
        property bool active: false
        property bool draft: false
        property bool lit: false
        property bool keyed: false // The keyboard-current row.
        property bool busy: false
        property bool pinned: false
        property bool renaming: false
        // Delete asked once (.chat-row.is-confirming): the time steps aside,
        // the actions stay, and Delete turns red with its lid lifted until a
        // second click, the pointer leaving or 3 s. Then, until OpenGhost answers,
        // the row takes no more clicks.
        property bool confirming: false
        property bool deleting: false
        property string time
        // The active fill, opaque; else a translucent wash over the backdrop.
        property color under: Theme.rowHover
        property real moved: 0 // Its place in the list changed (backdrop pieces follow).
        readonly property alias mark: mark
        readonly property alias field: field
        // Lit after a short wait, so a quick sweep down the list stays calm:
        // the room and the time after 50 ms, the actions after 50, 80 and
        // 110 ms; unlit at once (.chat-row:hover transition delays). The
        // field puts the room and the actions away at once.
        Later { id: roomy; when: (face.lit || face.confirming) && !face.renaming; after: 50 }
        Later { id: timeAway; when: face.lit; after: 50 }
        // The third action's own hover delay outranks the field's (CSS
        // specificity), so it steps aside 110 ms after the others.
        Later { id: renamingLate; when: face.renaming; after: 110 }
        signal pin()
        signal remove()
        signal rename()
        // The field closed: `keep` the text or not; `refocus` the row (Enter
        // or Escape in the focused field).
        signal renamed(bool keep, string text, bool refocus)
        x: 10
        width: parent ? parent.width - 20 : 0
        height: 32
        radius: 9
        color: renaming ? Theme.composerBg : active ? Theme.rowActive : "transparent"
        // The active row is a small card of its own (--row-active-lift).
        RowLift {
            visible: face.active && !face.renaming
            radius: 9
        }
        Rectangle { // :focus-visible, over the title's fade
            z: 1
            anchors.fill: parent
            radius: 9
            color: "transparent"
            border.width: 2
            border.color: Theme.alpha(Theme.strong, 0.35)
            visible: face.keyed
        }
        // Renaming: an inset 1 px line at .18 that comes in from clear (chat-rename-in, 0.3 s).
        Rectangle {
            id: rim
            property real shown: 0
            anchors.fill: parent
            radius: 9
            color: "transparent"
            border.width: 1
            border.color: Theme.alpha(Theme.strong, 0.18 * shown)
            visible: face.renaming
            NumberAnimation {
                id: rimIn
                target: rim
                property: "shown"
                from: 0
                to: 1
                duration: 300
                easing.type: Easing.Bezier
                easing.bezierCurve: Theme.motion
            }
        }
        HoverHandler {
            enabled: face.renaming
            cursorShape: Qt.IBeamCursor
        }
        onRenamingChanged: {
            rimIn.stop()
            rim.shown = renaming && Theme.reducedMotion ? 1 : 0
            if (renaming && !Theme.reducedMotion)
                rimIn.start()
        }
        Item { // .chat-mark
            id: mark
            x: 10
            y: 7
            width: 18
            height: 18
            Rectangle { // .chat-dot, at 6.5 px snapped down and right as Chromium does
                x: 7
                y: 7
                width: 5
                height: 5
                radius: 2.5
                color: Theme.alpha(Theme.strong, 0.28)
                opacity: face.busy ? 0 : 1
                scale: face.busy ? 0.2 : 1
                Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
                Behavior on scale {
                    enabled: !Theme.reducedMotion
                    NumberAnimation {
                        duration: 350
                        easing.type: Easing.Bezier
                        easing.bezierCurve: Theme.motion
                    }
                }
            }
        }

        // The title drawn. Another title for the same chat, from a rename or
        // from OpenGhost naming it, writes itself in: each letter rises out of a
        // 2 px blur 0.35 em low over 0.36 s, a moment after the one before,
        // the whole of it within 0.42 s (chat-list.js write(), .chat-letter);
        // then it is plain text again. Not with reduced motion.
        property string shown
        property string seenKey
        property var letters: []
        function observe() {
            const write = !face.pooled && face.key !== "" && face.key === seenKey
                          && shown !== "" && face.title !== shown && !Theme.reducedMotion
            seenKey = face.key
            shown = face.title
            letters = write ? spell(face.title) : []
            if (write)
                writing.restart()
            else
                writing.stop()
        }
        function spell(text) {
            const chars = Array.from(text)
            const step = Math.min(18, 420 / Math.max(1, chars.length))
            let at = 0
            return chars.map((c, k) => {
                const letter = { text: c, at: at, delay: Math.round(k * step) }
                at += c.length
                return letter
            })
        }
        onTitleChanged: observe()
        onKeyChanged: observe()
        onPooledChanged: if (pooled) { writing.stop(); letters = [] }
        Component.onCompleted: observe()
        Timer { id: writing; interval: 360 + 420 + 60; onTriggered: face.letters = [] }
        TextInput { // Where each letter of the title lies.
            id: ruler
            visible: false
            text: face.shown
            font: titleText.font
        }
        Text {
            id: titleText
            x: 38
            y: (face.height - 21) / 2 + Theme.halfLeading(font, 21)
            width: (face.draft ? face.width - 8 : meta.x - 10) - x
            text: face.shown
            textFormat: Text.PlainText
            clip: true
            font.pixelSize: 14
            color: face.draft ? Theme.secondary : Theme.text
            visible: !face.renaming && face.letters.length === 0
        }
        Item {
            x: titleText.x
            width: titleText.width
            height: face.height
            clip: true
            visible: !face.renaming && face.letters.length > 0
            Repeater {
                model: face.letters
                Text {
                    id: letter
                    required property var modelData
                    property real p: 0
                    x: ruler.positionToRectangle(modelData.at).x
                    // top: 0.35em → auto does not interpolate: it drops at half.
                    y: titleText.y + (p < 0.5 ? 0.35 * 14 : 0)
                    text: modelData.text
                    textFormat: Text.PlainText
                    font: titleText.font
                    color: titleText.color
                    opacity: p
                    // Out of a 2 px blur (MultiEffect: σ ≈ 4.3 px at blur 1).
                    layer.enabled: p > 0 && p < 1
                    layer.effect: MultiEffect {
                        blurEnabled: true
                        blurMax: 16
                        blur: 2 * (1 - letter.p) / 4.3
                    }
                    SequentialAnimation on p {
                        PauseAnimation { duration: letter.modelData.delay }
                        NumberAnimation {
                            from: 0
                            to: 1
                            duration: 360
                            easing.type: Easing.Bezier
                            easing.bezierCurve: Theme.motion
                        }
                    }
                }
            }
        }
        // A long title fades out over its last 20 px instead of an ellipsis:
        // into the active fill, or into the backdrop where the row lies in
        // the window, under its share of the hover glide.
        Rectangle {
            x: titleText.x + titleText.width - width
            width: 20
            height: parent.height
            visible: !face.renaming && face.active && ruler.implicitWidth > titleText.width
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Theme.alpha(face.under, 0) }
                GradientStop { position: 1; color: face.under }
            }
        }
        Backdrop {
            id: titleEnd
            x: titleText.x + titleText.width - width
            width: 20
            height: parent.height
            visible: !face.renaming && !face.active && ruler.implicitWidth > titleText.width
            box: Qt.size(Window.width, Window.height)
            origin: {
                void face.moved; void x; void face.y
                return visible ? titleEnd.mapToItem(null, 0, 0) : Qt.point(0, 0)
            }
            mask: 3
            wash: face.under
        }

        // .chat-rename: the title as the field's text, all of it selected.
        // Enter or a press elsewhere keeps the new name, Escape the old one;
        // focus leaving counts only while the window is active, so stepping
        // over to another app leaves the field waiting.
        TextInput {
            id: field
            objectName: "renameField"
            property string original
            x: 38
            width: Math.max(0, meta.x - 10 - x)
            height: face.height
            visible: face.renaming
            clip: true
            // The text stays where the title was (Chromium centres the 21 px line).
            topPadding: titleText.y
            font: titleText.font
            color: Theme.text
            selectionColor: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, 0.3)
            // Opaque: Qt draws selected text over its unselected pass.
            selectedTextColor: Qt.tint(Qt.tint(Theme.composerBg, selectionColor), Theme.text)
            selectByMouse: true
            // As Chromium's caret: none over a selection, blinking otherwise.
            cursorDelegate: Rectangle {
                id: caret
                property bool lit: true
                width: 1
                color: Theme.text
                visible: field.cursorVisible && field.selectionStart === field.selectionEnd && lit
                onXChanged: { lit = true; blink.restart() }
                Timer {
                    id: blink
                    interval: Math.max(250, Qt.styleHints.cursorFlashTime / 2)
                    running: field.cursorVisible && Qt.styleHints.cursorFlashTime > 0
                    repeat: true
                    onTriggered: caret.lit = !caret.lit
                }
            }
            // The native editor's bound (120 characters), never cutting the title it had.
            maximumLength: Math.max(120, original.length)
            inputMethodHints: Qt.ImhNoPredictiveText
            onVisibleChanged: {
                if (!visible)
                    return
                original = face.title
                text = original
                forceActiveFocus()
                selectAll()
            }
            function finish(keep, refocus) {
                if (!face.renaming)
                    return
                const focused = activeFocus
                focus = false // A closed field never takes the row's focus back.
                face.renamed(keep, text, refocus && focused)
            }
            onActiveFocusChanged: {
                if (!activeFocus)
                    Qt.callLater(() => {
                        if (face.renaming && !field.activeFocus && field.Window.active)
                            field.finish(true, false)
                    })
            }
            Keys.priority: Keys.AfterItem
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
                    return
                event.accepted = true // The list never sees the field's keys.
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                    finish(true, true)
                else if (event.key === Qt.Key_Escape)
                    finish(false, true)
            }
            onAccepted: finish(true, true)
        }

        Item { // .chat-meta
            id: meta
            visible: !face.draft
            anchors.right: parent.right
            anchors.rightMargin: 8
            // min-width 26, or 104 to make room for the actions; none while
            // renaming, the time then standing aside.
            width: face.renaming ? 0 : Math.max(roomy.value ? 104 : 26, time.implicitWidth)
            height: parent.height
            Behavior on width {
                enabled: !Theme.reducedMotion
                NumberAnimation {
                    duration: 360
                    easing.type: Easing.Bezier
                    easing.bezierCurve: Theme.motion
                }
            }
            Text { // .chat-time
                id: time
                anchors.right: parent.right
                // As the heading: Chromium's rounded metrics lift it 1 px.
                y: (parent.height - 19.5) / 2 + Theme.halfLeading(font, 19.5) - 1
                text: face.time
                font.pixelSize: 13
                font.features: { "tnum": 1 }
                color: Theme.alpha(Theme.strong, 0.45)
                opacity: timeAway.value || face.confirming || face.renaming ? 0 : 1
                scale: timeAway.value ? 0.85 : 1
                Behavior on opacity {
                    enabled: !Theme.reducedMotion
                    NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
                }
                Behavior on scale {
                    enabled: !Theme.reducedMotion
                    NumberAnimation {
                        duration: 300
                        easing.type: Easing.Bezier
                        easing.bezierCurve: Theme.motion
                    }
                }
            }
            Row { // .chat-actions: OpenGhost has no lock, so rename, pin, delete.
                anchors.right: parent.right
                anchors.rightMargin: -4
                anchors.verticalCenter: parent.verticalCenter
                Repeater {
                    model: ["pencil", "pin", "trash"]
                    Action {
                        id: rowAction
                        required property string modelData
                        required property int index
                        Later {
                            id: pop
                            when: (face.lit || face.confirming)
                                  && !(rowAction.index === 2 ? renamingLate.value : face.renaming)
                            after: 50 + 30 * rowAction.index
                        }
                        objectName: ["renameChat", "pinChat", "deleteChat"][index]
                        glyph: modelData
                        drawn: modelData === "pin"
                        on: modelData === "pin" && face.pinned
                        danger: modelData === "trash" && face.confirming
                        enabled: (face.lit || face.confirming) && !face.renaming && !face.deleting
                        Accessible.name: ["Rename", face.pinned ? "Unpin" : "Pin",
                                          face.confirming ? "Click again to delete"
                                                          : "Delete chat"][index]
                        ButtonTip { text: rowAction.Accessible.name }
                        opacity: pop.value ? 1 : 0
                        scale: pop.value ? 1 : 0.7
                        Behavior on opacity {
                            enabled: !Theme.reducedMotion
                            NumberAnimation { duration: 180; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
                        }
                        Behavior on scale {
                            enabled: !Theme.reducedMotion
                            NumberAnimation {
                                duration: 400
                                easing.type: Easing.Bezier
                                easing.bezierCurve: [0.34, 1.56, 0.64, 1, 1, 1]
                            }
                        }
                        // Reached for, the pencil tips forward and writes a
                        // short line (.glyph-pencil .pencil-body/.pencil-line).
                        Loader {
                            active: rowAction.modelData === "pencil"
                            anchors.fill: parent
                            sourceComponent: Item {
                                readonly property bool reached: rowAction.hovered && rowAction.enabled
                                property real tip: reached ? 1 : 0
                                property real line: reached ? 1 : 0
                                Behavior on tip {
                                    enabled: !Theme.reducedMotion
                                    NumberAnimation {
                                        duration: 400
                                        easing.type: Easing.Bezier
                                        easing.bezierCurve: [0.34, 1.56, 0.64, 1, 1, 1]
                                    }
                                }
                                Behavior on line {
                                    enabled: !Theme.reducedMotion
                                    NumberAnimation { duration: 360; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                                }
                                // translateX(3) rotate(-8deg) about (44, 76) of the 60-unit view.
                                PathIcon {
                                    objectName: "pencilBody"
                                    x: Math.ceil((rowAction.width - width) / 2)
                                    y: Math.ceil((rowAction.height - height) / 2)
                                    width: rowAction.iconSize
                                    height: rowAction.iconSize
                                    name: "pencil"
                                    color: rowAction.tint
                                    transform: [
                                        Rotation { origin.x: 14 * 15 / 60; origin.y: 46 * 15 / 60; angle: -8 * tip },
                                        Translate { x: 3 * 15 / 60 * tip }
                                    ]
                                }
                                PathIcon {
                                    objectName: "pencilLine"
                                    x: Math.ceil((rowAction.width - width) / 2)
                                    y: Math.ceil((rowAction.height - height) / 2)
                                    width: rowAction.iconSize
                                    height: rowAction.iconSize
                                    name: "pencil-line"
                                    reveal: line
                                    color: rowAction.tint
                                }
                            }
                        }
                        Loader {
                            active: rowAction.modelData === "trash"
                            anchors.fill: parent
                            sourceComponent: TrashGlyph { action: rowAction; lifted: face.confirming }
                        }
                        onClicked: modelData === "pencil" ? face.rename()
                                 : modelData === "pin" ? face.pin() : face.remove()
                    }
                }
            }
        }
    }

    // .sidebar-header: search and New Chat, 32 px, 10 px from the top.
    Item {
        id: header
        x: 10
        y: 10
        width: parent.width - 20
        height: 32

        // .sidebar-search: a 32 px lens that opens into a capsule on hover,
        // focus or a query (search-field.js).
        Item {
            id: slot
            width: header.width - 32 - 6
            height: 32
            property bool pointed: false // Hovered, after the intent delay.
            readonly property bool hot: shellHover.hovered || lens.hovered
            readonly property bool open: pointed || search.activeFocus || sidebar.searching
                                         || sidebar.opening
            Timer {
                id: intent
                interval: slot.hot ? 60 : 140
                onTriggered: slot.pointed = slot.hot
            }
            Spring {
                id: expand
                goal: slot.open ? 1 : 0
                k: slot.open ? 300 : 340
                c: slot.open ? 28 : 30
                step: 0.004
                slower: 0.005
            }
            // The capsule squashes up to 8 % with its opening speed (520/34).
            Spring {
                id: squash
                goal: Math.min(0.08, Math.abs(expand.velocity) * 0.012)
                k: 520
                c: 34
                step: 0.004
                within: 0.001
                slower: 0.01
            }
            // The focus ring tightens onto the field from a wider, clear one.
            property real ring: search.activeFocus ? 1 : 0
            Behavior on ring {
                enabled: !Theme.reducedMotion
                NumberAnimation { duration: 350; easing.type: Easing.Bezier; easing.bezierCurve: [0.2, 0.8, 0.2, 1, 1, 1] }
            }
            function smooth(v) { return v * v * (3 - 2 * v) }
            function clamp01(v) { return Math.min(1, Math.max(0, v)) }
            onHotChanged: intent.restart()
            readonly property real appear: smooth(clamp01(expand.value / 0.2))
            readonly property real shellWidth: Math.max(32, 32 + (width - 32) * expand.value)

            Item { // .sidebar-search-shell
                id: shell
                width: slot.shellWidth
                height: 32
                HoverHandler { id: shellHover }
                Item {
                    anchors.fill: parent
                    opacity: slot.appear
                    transform: Scale {
                        origin.x: 16
                        origin.y: 16
                        xScale: 0.8 + 0.2 * slot.appear
                        yScale: xScale * (1 - Math.max(0, squash.value))
                    }
                    // box-shadow: 0 1px 2px rgba(0, 0, 0, .28), outside the shell
                    // only: the shell is translucent over the backdrop.
                    BoxShadow {
                        anchors.fill: parent
                        radius: 16
                        offsetY: 1
                        blur: 2
                        knockout: true
                        color: Theme.alpha("black", 0.28 * Theme.shadow)
                    }
                    // rgba(var(--fg-rgb), .055 / .07 hovered / .08 focused) under a
                    // sheen; light, white at .5 / .66 / .85 with no sheen (1.3).
                    Rectangle {
                        anchors.fill: parent
                        radius: 16
                        color: Theme.light ? Qt.rgba(1, 1, 1, search.activeFocus ? 0.85 : slot.hot ? 0.66 : 0.5)
                                           : Theme.alpha(Theme.strong, search.activeFocus ? 0.08 : slot.hot ? 0.07 : 0.055)
                        Behavior on color { ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
                        Rectangle {
                            anchors.fill: parent
                            radius: 16
                            visible: !Theme.light
                            gradient: Gradient {
                                GradientStop { position: 0; color: Theme.alpha(Theme.strong, 0.035) }
                                GradientStop { position: 0.6; color: "transparent" }
                                GradientStop { position: 1; color: Theme.alpha("black", 0.06 * Theme.shadow) }
                            }
                        }
                    }
                    // The 1 px rim, brighter at the top.
                    Shape {
                        anchors.fill: parent
                        preferredRendererType: Shape.CurveRenderer
                        ShapePath {
                            strokeColor: "transparent"
                            fillRule: ShapePath.OddEvenFill
                            fillGradient: LinearGradient {
                                y2: 32
                                GradientStop { position: 0; color: Theme.light ? Qt.rgba(1, 1, 1, 0.95) : Theme.alpha(Theme.strong, 0.13) }
                                GradientStop { position: 0.45; color: Theme.light ? Qt.rgba(1, 1, 1, 0.4) : Theme.alpha(Theme.strong, 0.035) }
                                GradientStop { position: 0.75; color: Theme.light ? Qt.rgba(30 / 255, 48 / 255, 88 / 255, 0.05) : Theme.alpha(Theme.strong, 0.015) }
                                GradientStop { position: 1; color: Theme.light ? Qt.rgba(30 / 255, 48 / 255, 88 / 255, 0.1) : Theme.alpha(Theme.strong, 0.05) }
                            }
                            PathRectangle { width: shell.width; height: 32; radius: 16 }
                            PathRectangle { x: 1; y: 1; width: shell.width - 2; height: 30; radius: 15 }
                        }
                    }
                    // Focused: a 2.5 px ring at .16 around it, from 7 px clear.
                    Rectangle {
                        readonly property real spread: 7 - 4.5 * slot.ring
                        anchors.fill: parent
                        anchors.margins: -spread
                        radius: 16 + spread
                        color: "transparent"
                        border.width: spread
                        border.color: Theme.alpha(Theme.strong, 0.16 * slot.ring)
                        visible: slot.ring > 0
                    }
                }
            }
            AbstractButton { // .sidebar-search-button
                id: lens
                objectName: "searchButton"
                width: 32
                height: 32
                focusPolicy: Qt.TabFocus
                scale: slot.open ? 0.84 : 1
                Behavior on scale {
                    enabled: !Theme.reducedMotion
                    NumberAnimation {
                        duration: slot.open ? 500 : 200
                        easing.type: Easing.Bezier
                        easing.bezierCurve: slot.open ? [0.3, 1.5, 0.5, 1, 1, 1]
                                                      : [0.25, 0.1, 0.25, 1, 1, 1]
                    }
                }
                contentItem: Item {
                    opacity: lens.hovered || lens.visualFocus ? 0.85 : 0.55
                    Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
                    // On hover the lens tilts 14° about its centre.
                    Spring { id: tilt; goal: lens.hovered && !Theme.reducedMotion ? 1 : 0; k: 230; c: 27 }
                    Spring { id: lensPress; goal: lens.down ? 1 : 0; k: 230; c: 27 }
                    PathIcon {
                        anchors.centerIn: parent
                        anchors.verticalCenterOffset: 2 * lensPress.value * 22 / 60
                        width: 22
                        height: 22
                        opacity: 1 - 0.3 * lensPress.value
                        name: "search"
                        color: Theme.strong
                        transform: Rotation {
                            origin.x: 22 * 24.5 / 60
                            origin.y: 22 * 24.5 / 60
                            angle: -14 * tilt.value
                        }
                    }
                }
                background: Rectangle {
                    radius: 16
                    color: "transparent"
                    border.width: 2
                    border.color: Theme.alpha(Theme.strong, 0.35)
                    visible: lens.visualFocus
                }
                onClicked: sidebar.focusSearch()
            }
            TextField {
                id: search
                objectName: "search"
                x: 32
                width: slot.width - 32
                height: 32
                enabled: slot.open
                leftPadding: 2
                rightPadding: sidebar.searching ? 30 : 10
                topPadding: 2 // Centred 1 px lower, as Chromium centres the input's line.
                bottomPadding: 0
                verticalAlignment: TextInput.AlignVCenter
                font.pixelSize: 14
                color: Theme.text
                background: null
                selectionColor: Theme.selection
                inputMethodHints: Qt.ImhNoPredictiveText
                onTextChanged: sidebar.sessions.query = text
                onActiveFocusChanged: if (!activeFocus) sidebar.opening = false
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Escape) {
                        event.accepted = true
                        sidebar.endSearch()
                    } else if (event.key === Qt.Key_Down && chats.count) {
                        event.accepted = true
                        chats.currentIndex = chats.step(-1, 1)
                        chats.forceActiveFocus()
                    }
                }
                // The placeholder's letters rise into view as the capsule
                // reaches them (search-field.js render()).
                Row {
                    id: letters
                    x: 5 // The input's padding and the 3 px caret gap.
                    y: 1
                    height: parent.height
                    visible: !sidebar.searching
                    Repeater {
                        model: ["S", "e", "a", "r", "c", "h"]
                        Text {
                            id: letter
                            required property string modelData
                            readonly property real shown:
                                slot.smooth(slot.clamp01((slot.shellWidth - (32 + letters.x + x + width / 2)
                                                          - 2) / 56))
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData
                            font: search.font
                            color: Theme.muted
                            opacity: shown
                            transform: Translate { y: (1 - shown) * 4 }
                            // Out of a 3 px blur (MultiEffect: σ ≈ 4.3 px at blur 1).
                            layer.enabled: shown > 0 && shown < 1
                            layer.effect: MultiEffect {
                                blurEnabled: true
                                blurMax: 16
                                blur: 3 * (1 - letter.shown) / 4.3
                            }
                        }
                    }
                }
            }
            // .sidebar-search-clear: a 16 px disc with a cross, with a query.
            AbstractButton {
                id: clear
                objectName: "clearSearch"
                x: slot.width - 4 - width
                y: 4
                width: 24
                height: 24
                focusPolicy: Qt.NoFocus
                enabled: sidebar.searching
                opacity: sidebar.searching ? 1 : 0
                scale: sidebar.searching ? 1 : 0.4
                Behavior on opacity { NumberAnimation { duration: sidebar.searching ? 150 : 120 } }
                Behavior on scale {
                    enabled: !Theme.reducedMotion
                    NumberAnimation {
                        duration: sidebar.searching ? 450 : 180
                        easing.type: Easing.Bezier
                        easing.bezierCurve: sidebar.searching ? [0.34, 1.56, 0.64, 1, 1, 1]
                                                              : [0.25, 0.1, 0.25, 1, 1, 1]
                    }
                }
                contentItem: Item {
                    Rectangle {
                        anchors.centerIn: parent
                        width: 16
                        height: 16
                        radius: 8
                        color: Theme.alpha(Theme.strong, clear.hovered ? 0.48 : 0.3)
                        Behavior on color { ColorAnimation { duration: 150; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
                    }
                    PathIcon {
                        anchors.centerIn: parent
                        width: 8
                        height: 8
                        name: "clear"
                        color: Theme.appBg
                    }
                }
                background: null
                onClicked: search.text = ""
            }
        }
        // .sidebar-new-chat (add-button.js)
        AbstractButton {
            id: newChat
            objectName: "newChat"
            x: header.width - 32
            width: 32
            height: 32
            focusPolicy: Qt.TabFocus
            enabled: sidebar.frontend.canSwitch
            Accessible.name: "New chat"
            ButtonTip { text: newChat.Accessible.name }
            contentItem: Item {
                opacity: !newChat.enabled ? 0.25 : newChat.hovered || newChat.visualFocus ? 0.85 : 0.55
                Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
                Spring { id: turn; goal: newChat.hovered && newChat.enabled && !Theme.reducedMotion ? 1 : 0; k: 170; c: 22 }
                Spring { id: newPress; goal: newChat.down ? 1 : 0; k: 230; c: 27 }
                PathIcon { // A quarter turn on hover.
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: 2 * newPress.value * 22 / 60
                    width: 22
                    height: 22
                    opacity: 1 - 0.3 * newPress.value
                    name: "new-chat"
                    color: Theme.strong
                    rotation: 90 * turn.value
                }
            }
            background: Rectangle {
                radius: 8
                color: "transparent"
                border.width: 2
                border.color: Theme.alpha(Theme.strong, 0.35)
                visible: newChat.visualFocus
            }
            onClicked: sidebar.newChat()
        }
    }

    // The draft's row, placed first in its folder while that is listed.
    property Item draftSlot: null
    RowFace {
        id: draftRow
        objectName: "draftRow"
        parent: sidebar.draftSlot
        visible: sidebar.drafting && sidebar.draftSlot !== null
        title: "New chat"
        draft: true
        active: true
        HoverHandler { onHoveredChanged: sidebar.hover(draftRow, hovered) }
    }

    // .chats: 14 px under the header and 8 px above the footer, spanning the
    // sidebar with rows 10 px inside; the list ends with 16 px of room.
    Item {
        id: chatsArea
        y: header.y + header.height + 14
        width: parent.width
        height: settings.y - 8 - y
        ListView {
            id: chats
            objectName: "chats"
            anchors.fill: parent
            clip: true
            reuseItems: true
            model: sidebar.sessions
            boundsBehavior: Flickable.StopAtBounds
            keyNavigationEnabled: true
            highlightFollowsCurrentItem: false
            // .chats-scrollbar: the list's height at the sidebar's right edge, over the fades.
            ScrollBar.vertical: OverlayScrollBar { z: 3; view: chats }
            // The next shown row from `from` in direction `by`, or `from`.
            function step(from, by) {
                for (let i = from + by; i >= 0 && i < count; i += by) {
                    if (!sidebar.hiddenAt(i))
                        return i
                }
                return from
            }
            onContentHeightChanged: sidebar.track()
            // chat-list.js: a row new to the list comes in from 8 px left
            // (420 ms), and rows that move glide to their places (440 ms).
            add: Theme.reducedMotion ? null : enter
            displaced: Theme.reducedMotion ? null : glide
            move: Theme.reducedMotion ? null : glide
            // A deleted row folds away to nothing as it fades and slips 10 px
            // left, and the rows after it rise with it (REMOVE: 320 ms);
            // gone at once with reduced motion.
            remove: Theme.reducedMotion ? null : removal
            removeDisplaced: Theme.reducedMotion ? null : rise
            Transition {
                id: enter
                NumberAnimation { property: "entry"; from: 0; to: 1; duration: 420; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                NumberAnimation { property: "x"; from: -8; to: 0; duration: 420; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
            Transition {
                id: glide
                NumberAnimation { property: "y"; duration: 440; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
            Transition {
                id: removal
                NumberAnimation { property: "gone"; from: 0; to: 1; duration: 320; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
            Transition {
                id: rise
                NumberAnimation { property: "y"; duration: 320; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }

            Rectangle { // .chats-glide
                parent: chats.contentItem
                z: -1
                x: 10
                y: glideY.value
                width: chats.width - 20
                height: Math.max(0, glideH.value)
                radius: 9
                color: Theme.rowHover
                opacity: Math.min(1, Math.max(0, glideO.value))
                visible: opacity > 0
            }

            // Pinned chats lead under their heading; the chats without a
            // folder follow (OpenGhost 1.3), opening the list when none is
            // pinned. Their group shows even with no chat listed.
            header: Column {
                width: chats.width
                Heading {
                    width: chats.width
                    visible: sidebar.sessions.pinnedCount > 0
                    text: "Pinned"
                    withAction: false
                }
                Loader {
                    active: sidebar.sessions.pinnedCount === 0 && sidebar.emptyHome
                    sourceComponent: emptyHome
                }
            }
            // Kept folders without a chat take their places by when they were
            // added (OpenGhost 1.2 keeps a folder whose last chat is deleted);
            // these come after the last listed one. A search hides them.
            // Without any folder, the Folders heading and "No folders yet".
            footer: Column {
                id: tail
                width: chats.width
                readonly property bool noFolders: (sidebar.searching ? sidebar.sessions.folders
                                                   : sidebar.sessions.arranged).length === 0
                Loader {
                    active: tail.noFolders && sidebar.sessions.pinnedCount > 0 && sidebar.emptyHome
                    sourceComponent: emptyHome
                }
                Item {
                    width: 1
                    height: tail.noFolders && sidebar.headingGap ? 12 : 0
                }
                Heading {
                    width: chats.width
                    visible: tail.noFolders
                    text: "Folders"
                    onAct: sidebar.folderWanted()
                }
                Repeater {
                    model: sidebar.searching ? []
                           : (sidebar.sessions.arranged, sidebar.sessions.emptyBefore(""))
                    delegate: keptFolder
                }
                Item {
                    width: chats.width
                    height: (hint.visible ? 30 : 0) + 16
                    Text { // .chats-empty.is-hint
                        id: hint
                        visible: tail.noFolders && (!sidebar.searching
                                 || sidebar.sessions.pinnedCount + sidebar.sessions.homeCount === 0)
                        x: 20
                        y: (30 - 20.25) / 2 + Theme.halfLeading(font, 20.25)
                        text: sidebar.searching ? "Nothing found" : "No folders yet"
                        font.pointSize: Theme.points(13.5)
                        color: Theme.alpha(Theme.strong, 0.35)
                    }
                }
            }
            // The chats without a folder while none is listed: their group,
            // empty but for New Chat's draft when it waits there.
            Component {
                id: emptyHome
                Item {
                    id: homeSlot
                    width: chats.width
                    height: children.length > 0 ? children[0].height : 0
                    Component.onCompleted: Qt.callLater(() => {
                        if (homeSlot && homeSlot.children.length === 0)
                            folderGroup.createObject(homeSlot, { section: "home:", empty: true })
                    })
                }
            }
            Component {
                id: keptFolder
                Item {
                    id: kept
                    required property string modelData
                    width: chats.width
                    height: children.length > 0 ? children[0].height : 0
                    // Made once the group this sits in is complete: a
                    // component cannot start an instance inside its own.
                    Component.onCompleted: Qt.callLater(() => {
                        if (kept && kept.children.length === 0)
                            folderGroup.createObject(kept, { section: kept.modelData, empty: true })
                    })
                }
            }
            section.property: "group"
            section.delegate: folderGroup
            Component {
                id: folderGroup
                Column {
                    id: group
                    required property string section
                    // A kept folder without chats, placed by when it was added.
                    property bool empty: false
                    readonly property bool pinnedGroup: section === "pinned:"
                    // OpenGhost 1.3's chats without a folder: a speech bubble
                    // for the folder, New chat and nothing to delete.
                    readonly property bool homeGroup: section === "home:"
                    readonly property int place: (sidebar.searching ? sidebar.sessions.folders
                                                  : sidebar.sessions.arranged).indexOf(section)
                    readonly property bool open: !sidebar.isCollapsed(section)
                    // Delete Folder: asking (.chats-folder.is-confirming), then
                    // folding away as 1.2's section does (REMOVE).
                    readonly property bool confirming: sidebar.confirmingFolder === section
                    readonly property bool leaving: sidebar.leaving[section] === true
                    property real gone: leaving ? 1 : 0
                    Behavior on gone {
                        enabled: !Theme.reducedMotion
                        NumberAnimation { duration: 320; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                    }
                    width: chats.width
                    // The kept folders without chats 1.2 orders before this one.
                    Repeater {
                        model: group.empty || group.pinnedGroup || group.homeGroup
                               || sidebar.searching ? []
                               : (sidebar.sessions.arranged,
                                  sidebar.sessions.emptyBefore(group.section))
                        delegate: keptFolder
                    }
                    readonly property bool first: !group.pinnedGroup && !group.homeGroup
                                                   && group.place === 0
                    // After the pinned chats, the chats without a folder
                    // when none is listed (and so has no group of its own).
                    Loader {
                        active: group.first && sidebar.sessions.pinnedCount > 0 && sidebar.emptyHome
                        sourceComponent: emptyHome
                    }
                    // The chats without a folder 12 px below the pinned ones;
                    // the Folders heading 12 px below what comes before it.
                    Item {
                        width: 1
                        height: (group.homeGroup && sidebar.sessions.pinnedCount > 0)
                                || (group.first && sidebar.headingGap) ? 12 : 0
                    }
                    Heading {
                        width: group.width
                        visible: group.first
                        text: "Folders"
                        onAct: sidebar.folderWanted()
                    }
                    Column {
                        width: group.width
                        height: implicitHeight * (1 - group.gone)
                        clip: group.gone > 0
                        opacity: 1 - group.gone
                        transform: Translate { x: -10 * group.gone }
                        // .chats-folder + .chats-folder: 4 px apart.
                        Item {
                            width: 1
                            height: !group.pinnedGroup && group.place > 0 ? 4 : 0
                        }
                        // .chats-folder-head: the folder, its name, and on hover New
                        // chat here and Delete folder.
                        AbstractButton {
                            id: head
                            objectName: "folderHead"
                            visible: !group.pinnedGroup
                            width: group.width
                            height: 34
                            focusPolicy: Qt.NoFocus
                            hoverEnabled: true
                            containmentMask: headFace
                            background: null
                            contentItem: null
                            enabled: !group.leaving
                            onHoveredChanged: {
                                sidebar.hover(headFace, hovered)
                                if (!hovered)
                                    sidebar.keepFolder(group.section)
                            }
                            onClicked: sidebar.toggleFolder(group.section)
                            Item {
                                id: headFace
                                x: 10
                                width: parent.width - 20
                                height: 32
                                PathIcon { // .chats-folder-icon: 17 px in 18, snapped as Chromium does
                                    x: 11
                                    y: 8
                                    width: 17
                                    height: 17
                                    visible: !group.homeGroup
                                    readonly property var folding: sidebar.folding(group.section)
                                    name: "folder"
                                    morphTo: "folder-shut"
                                    morph: folding ? folding.front : group.open ? 0 : 1
                                    color: Theme.alpha(Theme.strong, head.hovered ? 0.85 : 0.62)
                                    Behavior on color { ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
                                }
                                // The chats' bubble (glyphs.js bubble): empty while
                                // they show, three dots filling it one after
                                // another as they fold away.
                                PathIcon {
                                    id: bubble
                                    x: 11
                                    y: 8
                                    width: 17
                                    height: 17
                                    visible: group.homeGroup
                                    readonly property var folding: sidebar.folding(group.section)
                                    readonly property bool shut: folding ? folding.shut : !group.open
                                    name: "bubble"
                                    color: Theme.alpha(Theme.strong, head.hovered ? 0.85 : 0.62)
                                    Behavior on color { ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
                                    Repeater {
                                        model: [20.5, 30, 39.5]
                                        Rectangle { // .bubble-dot: a 6.5 unit round cap
                                            id: dot
                                            required property real modelData
                                            required property int index
                                            readonly property real unit: bubble.width / 60
                                            width: 6.5 * unit
                                            height: width
                                            radius: width / 2
                                            x: modelData * unit - width / 2
                                            y: 27.5 * unit - height / 2
                                            color: bubble.color
                                            opacity: bubble.shut ? 1 : 0
                                            scale: bubble.shut ? 1 : 0.2
                                            Behavior on opacity {
                                                enabled: !Theme.reducedMotion
                                                SequentialAnimation {
                                                    PauseAnimation { duration: bubble.shut ? 50 * dot.index : 0 }
                                                    NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
                                                }
                                            }
                                            Behavior on scale {
                                                enabled: !Theme.reducedMotion
                                                SequentialAnimation {
                                                    PauseAnimation { duration: bubble.shut ? 50 * dot.index : 0 }
                                                    NumberAnimation {
                                                        duration: bubble.shut ? 450 : 300
                                                        easing.type: Easing.Bezier
                                                        easing.bezierCurve: bubble.shut ? [0.34, 1.56, 0.64, 1, 1, 1]
                                                                                        : Theme.motion
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                Text { // .chats-folder-name: the actions keep their room.
                                    x: 38
                                    y: (headFace.height - 21) / 2 + Theme.halfLeading(font, 21)
                                    width: headFace.width - 8 - (group.homeGroup ? 0 : 14 + 10)
                                           - 26 - 10 - x
                                    text: sidebar.name(group.section)
                                    elide: Text.ElideRight
                                    font.pixelSize: 14
                                    color: Theme.text
                                }
                                Repeater {
                                    model: group.homeGroup ? ["plus"] : ["plus", "trash"]
                                    Action {
                                        id: folderAction
                                        required property string modelData
                                        required property int index
                                        objectName: index === 0 ? "newChatHere" : "deleteFolder"
                                        // Delete overlaps New chat's gap by 8 px and
                                        // hangs 4 px into the head's padding; the
                                        // chats' New chat ends at the padding.
                                        x: group.homeGroup ? headFace.width - 8 - width
                                           : headFace.width - 8 + 4 - width - (index === 0 ? 28 : 0)
                                        y: 3
                                        glyph: modelData
                                        drawn: modelData !== "trash"
                                        rest: 0.62
                                        danger: index === 1 && group.confirming
                                        enabled: (head.hovered || group.confirming) && !group.leaving
                                        Accessible.name: index === 0
                                                         ? group.homeGroup ? "New chat"
                                                           : "New chat in " + sidebar.name(group.section)
                                                         : group.confirming
                                                           ? sidebar.folderLabel(group.section)
                                                           : "Delete folder"
                                        ButtonTip { text: folderAction.Accessible.name }
                                        // Delete follows 30 ms behind, both ways;
                                        // asking keeps both shown.
                                        property bool late: false
                                        readonly property bool shown: group.confirming
                                                                      || (index === 0 || Theme.reducedMotion
                                                                          ? head.hovered : late)
                                        Timer {
                                            interval: 30
                                            running: head.hovered !== folderAction.late
                                            onTriggered: folderAction.late = head.hovered
                                        }
                                        opacity: shown ? 1 : 0
                                        scale: shown ? 1 : 0.7
                                        Behavior on opacity {
                                            enabled: !Theme.reducedMotion
                                            NumberAnimation { duration: 150; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
                                        }
                                        Behavior on scale {
                                            enabled: !Theme.reducedMotion
                                            NumberAnimation {
                                                duration: 350
                                                easing.type: Easing.Bezier
                                                easing.bezierCurve: [0.34, 1.56, 0.64, 1, 1, 1]
                                            }
                                        }
                                        Loader {
                                            active: folderAction.modelData === "trash"
                                            anchors.fill: parent
                                            sourceComponent: TrashGlyph {
                                                action: folderAction
                                                lifted: group.confirming
                                            }
                                        }
                                        onClicked: {
                                            if (index === 0) {
                                                sidebar.newChatIn(group.section)
                                                return
                                            }
                                            sidebar.askDeleteFolder(group.section)
                                            if (group.leaving)
                                                sidebar.hover(headFace, false)
                                        }
                                    }
                                }
                            }
                        }
                        // The draft, first in its folder.
                        Item {
                            id: holder
                            readonly property bool owns: !group.pinnedGroup
                                                         && group.section === sidebar.home
                            width: group.width
                            height: owns && sidebar.drafting ? 34 : 0
                            onOwnsChanged: {
                                if (owns)
                                    sidebar.draftSlot = holder
                                else if (sidebar.draftSlot === holder)
                                    sidebar.draftSlot = null
                            }
                            Component.onCompleted: if (owns) sidebar.draftSlot = holder
                            Component.onDestruction: if (sidebar.draftSlot === holder) sidebar.draftSlot = null
                        }
                        Item { // .chats-empty: a kept folder's body without chats
                            objectName: "folderEmpty"
                            width: group.width
                            height: visible ? 30 : 0
                            visible: group.empty && !group.homeGroup && group.open
                                     && holder.height === 0
                            Text {
                                x: 48
                                y: (30 - 20.25) / 2 + Theme.halfLeading(font, 20.25)
                                text: "No chats yet"
                                font.pointSize: Theme.points(13.5)
                                color: Theme.alpha(Theme.strong, 0.35)
                            }
                        }
                    }
                }
            }
            delegate: AbstractButton {
                id: row
                required property int index
                required property string sessionId
                required property string title
                required property string folder
                required property double updated
                required property bool pinned
                objectName: "chat-" + sessionId
                readonly property bool highlighted: sessionId === sidebar.frontend.session
                readonly property bool hidden: sidebar.hiddenAt(index)
                readonly property bool keyed: ListView.isCurrentItem && chats.activeFocus
                // Its folder's collapse: the closing body cuts the rows off
                // from the end (.chats-folder-body), which fade as it goes.
                readonly property var folding: index >= sidebar.sessions.pinnedCount
                                                ? sidebar.folding(folder) : null
                readonly property real fold: folding ? folding.fold : hidden ? 1 : 0
                // Removed, or folding away with its deleted folder.
                readonly property real out: Math.max(gone, folding ? folding.gone : 0)
                readonly property var place: folding ? sidebar.run(index) : null
                width: ListView.view.width
                height: (1 - out) * (fold <= 0 ? 34 : fold >= 1 || !place ? 0
                        : Math.max(0, Math.min(34, 34 * (place.length * (1 - fold) - place.place))))
                visible: height > 0
                clip: fold > 0 || out > 0
                property real entry: 1 // The add transition's.
                property real gone: 0 // The remove transition's.
                transform: Translate { x: -10 * row.out }
                ListView.onPooled: face.pooled = true
                ListView.onReused: {
                    entry = 1
                    gone = 0
                    face.pooled = false
                }
                // A row on its way out is no longer reached for.
                ListView.onRemove: {
                    if (sidebar.hoverItem === face)
                        sidebar.hoverItem = null
                }
                Component.onDestruction: {
                    if (sidebar.renameField === face.field)
                        sidebar.renameField = null
                    if (sidebar.renamingId === sessionId)
                        sidebar.renamingId = ""
                }
                opacity: (folding ? folding.shade : 1) * entry * (1 - out)
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                containmentMask: face
                background: null
                contentItem: null
                onHoveredChanged: {
                    sidebar.hover(face, hovered && out === 0)
                    if (!hovered)
                        sidebar.keepChat(sessionId)
                }
                onYChanged: if (sidebar.hoverItem === face) sidebar.track()
                onClicked: if (out === 0 && (sidebar.frontend.canSwitch || highlighted))
                               sidebar.frontend.open(sessionId)
                RowFace {
                    id: face
                    objectName: "rowFace"
                    key: row.sessionId
                    title: row.title
                    active: row.highlighted
                    lit: row.hovered || row.keyed
                    keyed: row.keyed && !renaming
                    busy: busy.on
                    pinned: row.pinned
                    renaming: sidebar.renamingId === row.sessionId
                    deleting: sidebar.deleting[row.sessionId] === true
                    confirming: sidebar.confirmingId === row.sessionId || deleting
                    time: sidebar.ago(row.updated, sidebar.now)
                    under: sidebar.under(face, row.highlighted)
                    moved: row.y - chats.contentY - 10 * row.out
                    onRenamingChanged: {
                        if (renaming)
                            sidebar.renameField = field
                        else if (sidebar.renameField === field)
                            sidebar.renameField = null
                    }
                    onRename: sidebar.startRename(row.sessionId, row.index)
                    // Enter or Escape gives the row its focus back, as
                    // OpenGhost does; the list shows the confirmed answer.
                    onRenamed: function(keep, text, refocus) {
                        sidebar.finishRename(row.sessionId, keep, text, row.title)
                        if (refocus) {
                            chats.currentIndex = row.index
                            chats.forceActiveFocus()
                        }
                    }
                    onPin: sidebar.sessions.togglePin(row.sessionId)
                    onRemove: sidebar.askDelete(row.sessionId)
                }
                // A running chat's mark (chat-list.js): its Ghost pops in
                // (420 ms) and shrinks away (220 ms) when the run ends;
                // with reduced motion, even turned on mid-way, at once.
                Item {
                    id: busy
                    parent: face.mark
                    readonly property bool on: row.highlighted && sidebar.frontend.busy
                    property real presence: 0
                    anchors.fill: parent
                    Component.onCompleted: presence = on ? 1 : 0
                    onOnChanged: {
                        popIn.stop()
                        popOut.stop()
                        if (Theme.reducedMotion)
                            presence = on ? 1 : 0
                        else
                            (on ? popIn : popOut).start()
                    }
                    NumberAnimation {
                        id: popIn
                        target: busy
                        property: "presence"
                        to: 1
                        duration: 420
                        easing.type: Easing.Bezier
                        easing.bezierCurve: [0.34, 1.56, 0.64, 1, 1, 1]
                    }
                    NumberAnimation {
                        id: popOut
                        target: busy
                        property: "presence"
                        to: 0
                        duration: 220
                        easing.type: Easing.InQuad
                    }
                    Connections {
                        target: Theme
                        enabled: popIn.running || popOut.running
                        function onReducedMotionChanged() {
                            if (!Theme.reducedMotion)
                                return
                            popIn.stop()
                            popOut.stop()
                            busy.presence = busy.on ? 1 : 0
                        }
                    }
                    visible: presence > 0
                    Ghost {
                        objectName: "busyMark"
                        x: 1.5
                        y: 0.5
                        width: 15
                        height: 16.4
                        scale: busy.on ? 0.3 + 0.7 * busy.presence : 0.4 + 0.6 * busy.presence
                        opacity: Math.min(1, busy.presence)
                        running: busy.visible
                        reducedMotion: Theme.reducedMotion
                        color: Theme.ghostColor
                        eyeColor: Theme.appBg
                    }
                }
            }
            Keys.onPressed: function(event) {
                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                     || event.key === Qt.Key_Space) && currentItem) {
                    event.accepted = true
                    sidebar.frontend.open(currentItem.sessionId)
                } else if (event.key === Qt.Key_F2 && currentItem) {
                    event.accepted = true
                    sidebar.startRename(currentItem.sessionId, currentIndex)
                } else if (event.key === Qt.Key_Delete && currentItem) {
                    // The keyboard-current row's Delete, as Enter on 1.2's
                    // focused trash button: once to ask, again to delete.
                    event.accepted = true
                    sidebar.askDelete(currentItem.sessionId)
                } else if (event.key === Qt.Key_Escape) {
                    event.accepted = true
                    sidebar.endSearch()
                } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
                    event.accepted = true
                    currentIndex = step(currentIndex, event.key === Qt.Key_Down ? 1 : -1)
                    positionViewAtIndex(currentIndex, ListView.Contain)
                }
            }
        }
        // The list slips under the header and footer through 28 px fades
        // that come in once there is more past that edge. Each is a piece of
        // the window's backdrop, pinned to the window so it lines up with the
        // light around it (mask: #000 → transparent at 70 %, and back).
        Backdrop {
            id: topFade
            width: parent.width
            height: 28
            opacity: Math.min(1, Math.max(0, chats.contentY - chats.originY) / 28)
            visible: opacity > 0
            box: Qt.size(Window.width, Window.height)
            origin: { void sidebar.x; void chatsArea.y; return topFade.mapToItem(null, 0, 0) }
            mask: 1
        }
        Backdrop {
            id: bottomFade
            y: parent.height - height
            width: parent.width
            height: 28
            opacity: Math.min(1, Math.max(0, chats.originY + chats.contentHeight - chats.contentY
                                             - chats.height) / 28)
            visible: opacity > 0
            box: Qt.size(Window.width, Window.height)
            origin: { void sidebar.x; void chatsArea.y; void y; return bottomFade.mapToItem(null, 0, 0) }
            mask: 2
        }
    }

    // .sidebar-footer: Settings, a gear and its label (settings-button.js),
    // 18 px above the window's bottom.
    ToolButton {
        id: settings
        objectName: "settingsButton"
        x: 10
        y: parent.height - 18 - height
        implicitHeight: 32
        leftPadding: 5
        rightPadding: 12
        topPadding: 0
        bottomPadding: 0
        // The 2 px ring over the whole pill.
        background: Rectangle {
            radius: 16
            color: "transparent"
            border.width: 2
            border.color: Theme.alpha(Theme.strong, 0.35)
            visible: settings.visualFocus
        }
        contentItem: Row {
            spacing: 6
            opacity: settings.hovered || settings.visualFocus ? 0.85 : 0.55
            Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
            Spring { id: gearTurn; goal: settings.hovered && !Theme.reducedMotion ? 1 : 0; k: 170; c: 22 }
            Spring { id: gearPress; goal: settings.down ? 1 : 0; k: 230; c: 27 }
            PathIcon { // One tooth's turn on hover; a press sinks and fades it.
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: 2 * gearPress.value * 22 / 60
                width: 22
                height: 22
                opacity: 1 - 0.3 * gearPress.value
                name: "gear"
                color: Theme.strong
                rotation: 60 * gearTurn.value
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: "Settings"
                color: Theme.strong
            }
        }
        onClicked: sidebar.settingsRequested()
    }
}
