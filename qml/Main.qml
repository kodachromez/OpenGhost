import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import OpenGhost.Cpp

ApplicationWindow {
    id: window
    required property var frontend
    readonly property var settings: frontend.settings
    // OpenGhost's window (desktop/main.cjs): 1280×840, at least 760×540.
    width: 1280
    height: 840
    minimumWidth: 760
    minimumHeight: 540
    visible: true
    title: "OpenGhost"
    color: Theme.appBg
    // The window's backdrop (body: var(--frame), var(--app-bg)), under the
    // sidebar and around the chat panel; the app's contents lie over it.
    background: Backdrop {
        id: windowBackdrop
        objectName: "windowBackdrop"
        box: Qt.size(width, height)
    }
    // The body's type (--font-size): system-ui, which Chromium takes from
    // the desktop's font as Qt does, at 15 px.
    font.pixelSize: 15
    // OpenGhost's palette for every control.
    palette.window: Theme.appBg
    palette.windowText: Theme.text
    palette.base: Theme.composerBg
    palette.alternateBase: Theme.hover
    palette.text: Theme.text
    palette.button: Theme.composerBg
    palette.buttonText: Theme.text
    palette.brightText: Theme.strong
    // OpenGhost's selection blue is for selected text only (each text
    // control sets it); a highlighted control fills like its active row.
    palette.highlight: Theme.rowActive
    palette.highlightedText: Theme.strong
    palette.placeholderText: Theme.tertiary
    palette.light: Theme.hover
    palette.midlight: Theme.hover
    palette.mid: Theme.composerBorder
    palette.dark: Theme.chatBg
    palette.shadow: "black"
    palette.toolTipBase: Theme.composerBg
    palette.toolTipText: Theme.text
    palette.link: Theme.link

    // The run's working Ghost follows what the run does (chat.js): shown
    // while OpenGhost works, dismissed while the answer streams, and back after
    // 1.5 s of quiet text, when the model is likely writing a tool call.
    property bool working: false
    // The shown conversation's approvals as cards: OpenGhost's list
    // (WindowController::approvals) plus those folding away after it
    // resolved them. Opening the details once keeps them open on the next
    // cards, for this window only.
    ListModel { id: approvalModel; objectName: "approvalModel" }
    property bool approvalDetails: false
    readonly property bool approving: frontend.approvals.length > 0
    function syncApprovals() {
        const list = frontend.approvals, ids = {}
        for (let i = 0; i < list.length; ++i)
            ids[list[i].requestId] = list[i]
        const leaving = []
        for (let i = 0; i < approvalModel.count; ++i) {
            const shown = approvalModel.get(i), now = ids[shown.requestId]
            if (now) {
                approvalModel.setProperty(i, "approval", now)
                delete ids[shown.requestId]
            } else if (!shown.leaving) {
                leaving.push(shown.requestId)
            }
        }
        // Marked after the pass: a card that folds away at once leaves the list.
        for (const id of leaving)
            for (let i = 0; i < approvalModel.count; ++i)
                if (approvalModel.get(i).requestId === id)
                    approvalModel.setProperty(i, "leaving", true)
        for (let i = 0; i < list.length; ++i)
            if (ids[list[i].requestId])
                approvalModel.append({requestId: list[i].requestId, approval: list[i], leaving: false})
    }
    function dropApproval(requestId) {
        for (let i = 0; i < approvalModel.count; ++i)
            if (approvalModel.get(i).requestId === requestId && approvalModel.get(i).leaving) {
                approvalModel.remove(i)
                return
            }
    }
    Connections {
        target: window.frontend
        function onApprovalsChanged() { window.syncApprovals() }
        // Another conversation's cards never fold into this one.
        function onConversationReplaced() { approvalModel.clear(); window.syncApprovals() }
    }
    readonly property bool running: frontend.busy
    property int runs: 0 // Each run's working Ghost starts afresh.
    onRunningChanged: {
        quiet.stop()
        if (running)
            ++runs
        working = running
    }
    Timer {
        id: quiet
        interval: 1500
        onTriggered: if (window.frontend.busy) window.working = true
    }
    // Whether anything in the window can be seen (the live metrics clock).
    WindowExposure {
        id: exposure
        window: window
    }
    // Enter presses a focused button as Space does (ButtonKeys).
    ButtonKeys {
        window: window
    }

    onClosing: function(event) {
        event.accepted = false
        window.frontend.close()
    }
    Component.onCompleted: {
        Selection.watch(window) // Presses elsewhere end a reply's selection.
        composer.forceActiveFocus()
    }
    // The file chooser exists only while open. Its selection (local URLs, the
    // one place QML sees file paths) goes straight to the facade, which keeps
    // paths on the worker; unloading afterwards drops the selection.
    Loader {
        id: filePicker
        objectName: "filePicker"
        active: false
        sourceComponent: FileDialog {
            title: "Attach files"
            fileMode: FileDialog.OpenFiles
            onAccepted: {
                window.attach(selectedFiles)
                filePicker.finish()
            }
            onRejected: filePicker.finish()
        }
        function choose() {
            active = true
            item.open()
        }
        // Not from inside the dialog's own signal handler.
        function finish() { Qt.callLater(() => { filePicker.active = false }) }
    }

    // A local refusal (a pick, paste or drop); cleared by the next action.
    property string notice
    readonly property string fileRefusal: "Pasted and dropped files cannot be attached. Use "
        + "+ to choose files; nothing was attached."
    // The composer's chosen files, in order: token, name, size and whether
    // it is a picture, never a path. They leave only when OpenGhost accepts the
    // message that sent them.
    ListModel { id: composerFiles; objectName: "composerFiles" }
    function fileTokens() {
        const tokens = []
        for (let i = 0; i < composerFiles.count; ++i)
            tokens.push(composerFiles.get(i).token)
        return tokens
    }
    function fileList() {
        const files = []
        for (let i = 0; i < composerFiles.count; ++i) {
            const file = composerFiles.get(i)
            files.push({ token: file.token, name: file.name, size: file.size,
                         picture: file.picture })
        }
        return files
    }
    // A selection that does not fit the draft is refused whole, first here,
    // then by the worker's checks (also of pictures, with General's in a new
    // chat), and again when its answer arrives.
    function attach(urls) {
        let pictures = 0
        for (let i = 0; i < composerFiles.count; ++i)
            pictures += composerFiles.get(i).picture ? 1 : 0
        notice = window.frontend.pick(urls, 20 - composerFiles.count, pictures)
    }
    function removeFile(index) {
        window.frontend.release([composerFiles.get(index).token])
        composerFiles.remove(index)
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

    // The admitted submission and the draft it sent. Only OpenGhost's acceptance of
    // that submission removes the draft; edits inside it meanwhile (even back
    // to the same text) keep the whole composer. A gesture that is not
    // admitted changes none of this, and a refusal leaves the composer alone.
    property double pendingSubmission: 0
    property string pendingDraft
    property bool pendingIntact: true
    property var pendingFiles: [] // The submission's file tokens.
    // The reader's own message brings the conversation into view at the send
    // gesture, not at its acknowledgement, which can arrive after newer scrolling.
    function submit() {
        const text = composer.text
        const files = fileTokens()
        const submission = window.frontend.send(text, files)
        if (!submission)
            return
        pendingSubmission = submission
        pendingDraft = text
        pendingIntact = true
        pendingFiles = files
        notice = ""
        transcript.resume()
    }
    // Drafts of the conversations not shown, by session ID ("" is New Chat's
    // local draft): text and chosen files; memory only. At most 32 are kept:
    // leaving a 33rd drops the one left longest ago and releases its files.
    // The opened conversation's draft leaves the cache first, so reopening
    // the oldest never counts it against that bound.
    readonly property var drafts: new Map()
    // Sessions OpenGhost deleted: their drafts go with them, files released.
    readonly property var deleted: new Set()
    function forgetDraft(sessionId) {
        deleted.add(sessionId)
        const draft = drafts.get(sessionId)
        if (!draft)
            return
        window.frontend.release(draft.files.map(file => file.token))
        drafts.delete(sessionId)
    }
    function switchDraft(left) {
        const opened = window.frontend.session
        let draft = drafts.get(opened) ?? { text: "", files: [] }
        drafts.delete(opened)
        drafts.delete(left)
        if (deleted.has(left)) {
            // The shown chat was deleted: what was typed there goes with it.
            window.frontend.release(fileList().map(file => file.token))
        } else if (composer.text.length > 0 || composerFiles.count > 0) {
            const shown = { text: composer.text, files: fileList() }
            if (left === opened)
                draft = shown
            else
                drafts.set(left, shown)
        }
        if (drafts.size > 32) {
            const oldest = drafts.keys().next().value
            window.frontend.release(drafts.get(oldest).files.map(file => file.token))
            drafts.delete(oldest)
        }
        composer.text = draft.text
        composerFiles.clear()
        for (const file of draft.files)
            composerFiles.append(file)
        composer.cursorPosition = composer.length
        // Switching waits for every submission's answer, so none is pending here.
        pendingSubmission = 0
        pendingDraft = ""
        pendingFiles = []
        notice = ""
    }
    // Ask about a selection: a quote block after any quotes already at the
    // composer's start, inserted as the text that is sent.
    function quote(text) {
        const whole = text.replace(/\r\n?/g, "\n").replace(/\n{3,}/g, "\n\n").trim()
        // At most 6,000 UTF-16 units, never half of a surrogate pair.
        const split = /^[\uD800-\uDBFF][\uDC00-\uDFFF]$/.test(whole.substr(5999, 2))
        const body = whole.slice(0, split ? 5999 : 6000)
        if (!body)
            return
        const quoted = "\n" + body.split("\n").map(line => ("> " + line).replace(/\s+$/, ""))
                                                   .join("\n") + "\n\n "
        const lead = /^(?:\n(?:>[^\n]*\n)+\n )*/.exec(composer.text)[0].length
        composer.insert(lead, quoted)
        composer.cursorPosition = composer.length
        composer.forceActiveFocus()
    }
    function focusSearch() {
        sidebarOpen = true
        sidebar.focusSearch()
    }
    Connections {
        target: window.frontend
        function onAnswered() {
            window.working = false
            quiet.restart()
        }
        function onWorked() {
            quiet.stop()
            window.working = window.frontend.busy
        }
        function onAccepted(submission) {
            if (submission !== window.pendingSubmission)
                return
            const text = window.pendingDraft
            if (window.pendingIntact && composer.text.startsWith(text))
                composer.remove(0, text.length)
            // Only the files it sent; files chosen meanwhile stay.
            for (let i = composerFiles.count - 1; i >= 0; --i) {
                if (window.pendingFiles.indexOf(composerFiles.get(i).token) >= 0)
                    composerFiles.remove(i)
            }
            window.frontend.release(window.pendingFiles)
            window.pendingSubmission = 0
            window.pendingDraft = ""
            window.pendingFiles = []
        }
        function onFilesPicked(files, error) {
            if (error) {
                window.notice = error
            } else if (composerFiles.count + files.length > 20) {
                window.frontend.release(files.map(file => file.token))
                window.notice = "Attach at most 20 files to one message. No files were added."
            } else {
                for (const file of files)
                    composerFiles.append(file)
                window.notice = ""
            }
        }
        function onSessionRemoved(sessionId, deleted) {
            if (deleted)
                window.forgetDraft(sessionId)
        }
        // The composer changes owner only once the conversation has.
        function onConversationReplaced(left) {
            window.switchDraft(left)
            transcript.follow = true
            transcript.stick()
        }
    }
    Connections {
        target: window.settings
        function onModelWanted() {
            if (!settingsDialog.visible)
                modelStage.openStage(false)
        }
    }
    Shortcut {
        sequence: "Ctrl+K"
        enabled: !settingsDialog.visible
        onActivated: window.focusSearch()
    }

    // Ask/Copy for a selection in the conversation; one menu for every row.
    // A reply's selection is the reply's own (Selection); any other text,
    // `area`'s.
    Menu {
        id: selectionMenu
        objectName: "selectionMenu"
        property var area: null
        // Read when the menu opens: a long selection's text is made once.
        property string selected
        function show(target) {
            area = target
            selected = area ? window.frontend.selectedText(area.textDocument, area.selectionStart,
                                                         area.selectionEnd) : Selection.text
            popup()
        }
        onClosed: area = null
        MenuItem {
            text: "Copy"
            enabled: selectionMenu.selected.length > 0
            onTriggered: window.frontend.copy(selectionMenu.selected)
        }
        MenuItem {
            objectName: "askSelection"
            text: "Ask about selection"
            enabled: selectionMenu.selected.length > 0
            onTriggered: window.quote(selectionMenu.selected)
        }
    }

    // A reply's selection, let go: the veil over the feed (selection-focus.js)
    // and the same filter on the composer and the jump button (.is-veiled),
    // each on its own timing: in, the veil and the composer over 0.52 s and
    // the jump button over 0.45 s (motion easing); out, 0.42 s
    // (cubic-bezier(.4, 0, .2, 1)) and 0.45 s. With reduced motion the veil
    // and the jump button are at once; the composer still eases in (1.2's
    // .is-veiled rule outranks its reduced-motion one) and leaves at once.
    property real veilProgress: 0
    property real composerVeil: 0
    property real jumpVeil: 0
    NumberAnimation { id: veilMotion; target: window; property: "veilProgress"; easing.type: Easing.Bezier }
    NumberAnimation { id: composerMotion; target: window; property: "composerVeil"; easing.type: Easing.Bezier }
    NumberAnimation { id: jumpMotion; target: window; property: "jumpVeil"; easing.type: Easing.Bezier }
    function veilTo(animation, property, to, duration, curve) {
        animation.stop()
        if (duration <= 0) {
            window[property] = to
            return
        }
        animation.to = to
        animation.duration = duration
        animation.easing.bezierCurve = curve
        animation.start()
    }
    Connections {
        target: Selection
        function onShownChanged() {
            const shown = Selection.shown, reduced = Theme.reducedMotion
            const leave = [0.4, 0, 0.2, 1, 1, 1]
            window.veilTo(veilMotion, "veilProgress", shown ? 1 : 0, reduced ? 0 : shown ? 520 : 420,
                          shown ? Theme.motion : leave)
            window.veilTo(composerMotion, "composerVeil", shown ? 1 : 0,
                          shown ? 520 : reduced ? 0 : 420, shown ? Theme.motion : leave)
            window.veilTo(jumpMotion, "jumpVeil", shown ? 1 : 0, reduced ? 0 : 450, Theme.motion)
        }
    }
    // Copy takes a reply's selection wherever the focus is; a text field
    // with a selection of its own keeps its Copy (it takes the shortcut).
    Shortcut {
        sequences: [StandardKey.Copy]
        enabled: Selection.active
        onActivated: window.frontend.copy(Selection.text)
    }
    // A press anywhere else lets the menu and the veil go, and the
    // selection too unless the press is a button's or a scroll bar's
    // (selection-menu.js: a pointer press hides; one on text, empty space or
    // a text field moves or collapses the selection). The reply's own
    // presses are its SelectArea's.
    Connections {
        target: Selection
        function onPressed(x, y) {
            const toolbar = selectionToolbar.mapFromItem(null, x, y)
            if (selectionToolbar.visible && selectionToolbar.contains(toolbar))
                return
            Selection.hide()
            if (!Selection.keeps(window.contentItem, x, y))
                Selection.clear()
        }
    }
    // A resize lets the menu and the veil go (the selection stays).
    onWidthChanged: Selection.hide()
    onHeightChanged: Selection.hide()
    // The rows a selection spans, read for those not built, and the width
    // their diagrams are laid out at (ChatEntry's wide width).
    Binding {
        target: Selection
        property: "model"
        value: window.frontend ? window.frontend.transcript : null
    }
    // Rows drawn by another renderer (a frontend plugin turned on or off) show
    // other texts: a range in them is not kept.
    Connections {
        target: window.frontend ? window.frontend.frontendPlugins : null
        function onRenderersChanged() { Selection.clear() }
    }
    Binding {
        target: Selection
        property: "diagramWidth"
        value: Math.min(transcript.width - 56, 1320)
    }

    SettingsDialog {
        id: settingsDialog
        frontend: window.frontend
        anchors.centerIn: Overlay.overlay
        onClosed: composer.forceActiveFocus()
    }
    // A theme change's transition, over everything, popups too.
    ThemeReveal {
        objectName: "themeReveal"
        parent: Overlay.overlay
        anchors.fill: parent
        z: 1000
        reducedMotion: Theme.reducedMotion
    }

    // --sidebar-size: max(240 px, 17.3 %).
    readonly property real sidebarWidth: Math.max(240, width * 0.173)
    property bool sidebarOpen: true
    Sidebar {
        id: sidebar
        objectName: "sidebar"
        frontend: window.frontend
        width: window.sidebarWidth
        height: parent.height
        // The panel covers it while sliding over it.
        visible: window.sidebarOpen || panel.x > 8.5
        onDone: composer.forceActiveFocus()
        onSettingsRequested: settingsDialog.open()
        onFolderWanted: window.notice = "Folder management is not connected in this UI shell."
        onRefused: text => window.notice = text
    }
    // The built-in browser (browser-panel.js), only in the desktop build: a
    // card --chat-gap from the window's right edge, under the chat card, which
    // makes room for it while it is open (.app.is-browser-open .main).
    readonly property var browser: frontend.browser
    readonly property real browserRoom: width - (sidebarOpen ? sidebarWidth : 0) - 3 * 8
    readonly property int browserWidth: browser ? (browser.savedWidth, browser.widthFor(browserRoom)) : 0
    readonly property bool browserResizing: browserLoader.item ? browserLoader.item.resizing : false
    property real chatRight: browser && browser.open ? 16 + browserWidth : 8
    Behavior on chatRight {
        enabled: !Theme.reducedMotion && !window.browserResizing
        NumberAnimation { duration: 600; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
    }
    Loader {
        id: browserLoader
        objectName: "browserLoader"
        active: window.browser !== null
        x: window.width - 8 - window.browserWidth
        width: window.browserWidth
        height: window.height - 8
        Component.onCompleted: if (window.browser) setSource("BrowserPanel.qml", {
            browser: window.browser, frontend: window.frontend })
        onLoaded: item.room = Qt.binding(() => window.browserRoom)
    }
    // A press outside the panel takes the keyboard back from its pages.
    BrowserFocus { panel: browserLoader.item }
    // The chat panel (.main): flush with the sidebar and the window's top,
    // --chat-gap (8 px) inside the right and bottom edges, on its own
    // background, with the double contour: 1.5 px --contour-inner inside,
    // 1.5 px --contour-outer outside. Collapsing the sidebar slides it to 8 px (0.6 s).
    // Under the contour it lifts off the backdrop (--card-lift): dark 0 18px
    // 48px -18px rgba(0, 0, 0, .7); light 0 1px 2px rgba(30, 48, 88, .04)
    // and 0 18px 44px -20px rgba(30, 48, 88, .16). A negative spread shrinks
    // the shadow's box and its radius (12 − 18 is 0).
    BoxShadow {
        readonly property real spread: Theme.light ? 20 : 18
        x: panel.x + spread
        y: panel.y + spread
        width: panel.width - 2 * spread
        height: panel.height - 2 * spread
        offsetY: 18
        blur: Theme.light ? 44 : 48
        color: Theme.light ? Qt.rgba(30 / 255, 48 / 255, 88 / 255, 0.16) : Qt.rgba(0, 0, 0, 0.7)
    }
    BoxShadow {
        visible: Theme.light
        x: panel.x
        y: panel.y
        width: panel.width
        height: panel.height
        radius: panel.radius
        offsetY: 1
        blur: 2
        color: Qt.rgba(30 / 255, 48 / 255, 88 / 255, 0.04)
    }
    Rectangle {
        x: panel.x - 1.5
        y: panel.y - 1.5
        width: panel.width + 3
        height: panel.height + 3
        radius: panel.radius + 1.5
        antialiasing: true
        color: Theme.contourOuter
    }
    Rectangle {
        id: panel
        x: window.sidebarOpen ? window.sidebarWidth : 8
        width: window.width - x - window.chatRight
        height: window.height - 8
        radius: 12
        antialiasing: true
        // The inner contour, then the background inside it: a filled edge
        // blends its half pixel as Chromium's does; a 1.5 px border does not.
        color: Theme.contourInner
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1.5
            radius: parent.radius - 1.5
            antialiasing: true
            color: Theme.chatBg
        }
        Behavior on x {
            enabled: !Theme.reducedMotion
            NumberAnimation {
                duration: 600
                easing.type: Easing.Bezier
                easing.bezierCurve: Theme.motion
            }
        }
        // .sidebar-toggle: a 32 px icon button 10 px inside the corner.
        ToolButton {
            id: sidebarToggle
            objectName: "sidebarToggle"
            x: 10
            y: 10
            z: 3
            width: 32
            height: 32
            padding: 0
            Accessible.name: window.sidebarOpen ? "Hide sidebar" : "Show sidebar"
            ButtonTip { text: sidebarToggle.Accessible.name }
            // sidebar-toggle.js: the split glides to its place (120/20) and,
            // under the pointer, 3 units toward the other one (230/27); a
            // press sinks the glyph 2 units and fades it to .7.
            Spring { id: splitSpring; goal: window.sidebarOpen ? 0 : 1; k: 120; c: 20 }
            Spring {
                id: nudge
                k: 230
                c: 27
                goal: Theme.reducedMotion || !sidebarToggle.hovered ? 0 : window.sidebarOpen ? -1 : 1
            }
            Spring { id: togglePress; goal: sidebarToggle.down ? 1 : 0; k: 230; c: 27 }
            Component.onCompleted: splitSpring.snap()
            background: Rectangle {
                radius: 8
                color: "transparent"
                border.width: 2
                border.color: Theme.alpha(Theme.strong, 0.35)
                visible: sidebarToggle.visualFocus
            }
            contentItem: Item {
                opacity: sidebarToggle.hovered || sidebarToggle.visualFocus ? 0.85 : 0.55
                Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
                ToggleIcon {
                    objectName: "toggleGlyph"
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: 2 * togglePress.value * 22 / 60
                    width: 22
                    height: 22
                    split: 22 - 11 * splitSpring.value + 3 * nudge.value
                    opacity: 1 - 0.3 * togglePress.value
                    color: Theme.strong
                }
            }
            onClicked: window.sidebarOpen = !window.sidebarOpen
        }
        // .browser-toggle: the globe 10 px inside the top-right corner, in the
        // desktop build only. Hovering spins its meridian (170/19); open, the
        // globe fills (170/24); a dot pulses while the agent uses the browser.
        ToolButton {
            id: browserToggle
            objectName: "browserToggle"
            visible: window.browser !== null
            x: panel.width - 42
            y: 10
            z: 3
            width: 32
            height: 32
            padding: 0
            Accessible.name: window.browser && window.browser.open ? "Hide browser" : "Show browser"
            ButtonTip { text: browserToggle.Accessible.name }
            Spring { id: globeSpin; goal: !Theme.reducedMotion && browserToggle.hovered ? 1 : 0; k: 170; c: 19 }
            Spring {
                id: globeOpen
                goal: window.browser && window.browser.open ? 1 : 0
                k: 170
                c: 24
                Component.onCompleted: snap()
            }
            Spring { id: globePress; goal: browserToggle.down ? 1 : 0; k: 230; c: 27 }
            background: Rectangle {
                radius: 8
                color: "transparent"
                border.width: 2
                border.color: Theme.alpha(Theme.strong, 0.35)
                visible: browserToggle.visualFocus
            }
            contentItem: Item {
                opacity: browserToggle.hovered || browserToggle.visualFocus ? 0.85 : 0.55
                Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
                Item {
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: 2 * globePress.value * 22 / 60
                    width: 22
                    height: 22
                    opacity: 1 - 0.3 * globePress.value
                    GlobeIcon {
                        objectName: "globeGlyph"
                        anchors.fill: parent
                        spin: globeSpin.value
                        open: globeOpen.value
                        color: Theme.strong
                    }
                    // .live: r 6 at (83, 37) of the 30…90 view.
                    Rectangle {
                        objectName: "browserLive"
                        x: 53 * 22 / 60 - width / 2
                        y: 7 * 22 / 60 - height / 2
                        width: 12 * 22 / 60
                        height: width
                        radius: width / 2
                        color: Theme.strong
                        visible: window.browser !== null && window.browser.agent
                        SequentialAnimation on opacity {
                            running: !Theme.reducedMotion && window.browser !== null && window.browser.agent
                            loops: Animation.Infinite
                            NumberAnimation { to: 0.35; duration: 800; easing.type: Easing.InOutQuad }
                            NumberAnimation { to: 1; duration: 800; easing.type: Easing.InOutQuad }
                        }
                        SequentialAnimation on scale {
                            running: !Theme.reducedMotion && window.browser !== null && window.browser.agent
                            loops: Animation.Infinite
                            NumberAnimation { to: 0.75; duration: 800; easing.type: Easing.InOutQuad }
                            NumberAnimation { to: 1; duration: 800; easing.type: Easing.InOutQuad }
                        }
                    }
                }
            }
            onClicked: window.browser.toggle()
        }
        // The thread fills the panel and scrolls under the floating composer
        // (.thread-view); its list ends above the composer (--composer-space).
        Item {
            id: transcriptArea
            anchors.fill: parent
            ListView {
                id: transcript
                objectName: "transcript"
                anchors.fill: parent
                clip: true
                // Rows carry their own gaps (ChatEntry.gap).
                spacing: 0
                model: window.frontend.transcript
                boundsBehavior: Flickable.StopAtBounds
                highlightFollowsCurrentItem: false
                keyNavigationEnabled: false
                readonly property var frontend: window.frontend
                // scroll-padding-bottom (.thread): where a selection's
                // autoscroll starts above the composer (SelectArea).
                readonly property real scrollPadding: composerFrame.space + 16
                function showMenu(area) { selectionMenu.show(area) }
                function formatSize(bytes) { return window.formatSize(bytes) }
                // Follow new output while `follow` is set. Only the reader
                // changes it: a scroll gesture (drag, flick, wheel or scroll
                // bar) follows exactly when it leaves the view within 48 px of
                // the bottom; Jump to latest and a send resume following. Row
                // growth, shrinking, clamping and removal never change it.
                property bool follow: true
                readonly property bool scrolling: moving || scrollBar.pressed || wheelClock.running
                                                  || pad.held
                // Rows shown as a conversation opens play their entry motion,
                // as OpenGhost's freshly rendered thread does.
                property bool opening: false
                Timer {
                    id: opened
                    interval: 200
                    onTriggered: transcript.opening = false
                }
                // Otherwise the row at the top of the view (by key: its
                // delegate may be destroyed) stays where the reader left it
                // while rows above or within it change size or leave.
                property string anchorKey
                property real anchorOffset: 0
                function atEnd() {
                    return contentHeight - (contentY - originY) - height < 48
                }
                function mark() {
                    const row = itemAt(0, contentY)
                    anchorKey = row ? row.key : ""
                    anchorOffset = row ? contentY - row.y : 0
                }
                function keep() {
                    if (follow || scrolling)
                        return
                    const index = model.indexOf(anchorKey)
                    if (index < 0)
                        return mark()
                    if (!itemAtIndex(index))
                        positionViewAtIndex(index, ListView.Beginning)
                    const row = itemAtIndex(index)
                    if (!row)
                        return
                    const y = Math.max(originY, Math.min(row.y + anchorOffset,
                                                         originY + contentHeight - height))
                    if (Math.abs(contentY - y) > 0.5)
                        contentY = y
                }
                // Only when not already there: positioning rebuilds the rows
                // at the end, whose heights settle in the next layout pass,
                // and repositioning on each of those changes never ends.
                function stick() {
                    if (follow && !scrolling && !jumping
                            && contentHeight - (contentY - originY) - height >= 0.5) {
                        followClock.stop()
                        positionViewAtEnd()
                    }
                }
                // New output is followed on OpenGhost's 130/23 spring
                // (chat.js followBottom()), aimed at the end each frame. It
                // lands at once (stick()) with reduced motion and as a
                // conversation opens. Following is the reader's, as before:
                // the spring only moves while it is set and no gesture,
                // scroll bar drag or jump is under way, and a gesture stops it.
                function glide() {
                    if (Theme.reducedMotion || opening)
                        return stick()
                    if (follow && !scrolling && !jumping
                            && contentHeight - (contentY - originY) - height >= 0.5)
                        followClock.begin()
                }
                function resume() {
                    follow = true
                    glide()
                }
                FrameAnimation {
                    id: followClock
                    objectName: "followClock"
                    property real position
                    property real velocity
                    function begin() {
                        if (running)
                            return
                        position = transcript.contentY
                        velocity = 0
                        start()
                    }
                    onTriggered: {
                        if (!transcript.follow || transcript.scrolling || transcript.jumping
                                || Theme.reducedMotion || transcript.opening) {
                            stop()
                            return transcript.stick()
                        }
                        const end = transcript.originY + transcript.contentHeight - transcript.height
                        // Moved by something else (the list's own layout): from there.
                        if (Math.abs(transcript.contentY - position) > 1.5) {
                            position = transcript.contentY
                            velocity = 0
                        }
                        const dt = Math.min(Math.max(frameTime, 0), 0.05)
                        const steps = Math.max(1, Math.ceil(dt / 0.004)), h = dt / steps
                        for (let i = 0; i < steps; ++i) {
                            velocity += ((end - position) * 130 - velocity * 23) * h
                            position += velocity * h
                        }
                        if (position >= end) {
                            position = end
                            velocity = Math.min(0, velocity)
                        }
                        if (end - position < 0.5 && Math.abs(velocity) < 4) {
                            stop()
                            return transcript.positionViewAtEnd()
                        }
                        transcript.contentY = position
                    }
                }
                // Wheel turns (WheelScroll) glide as OpenGhost 1.2's do: each
                // adds its distance to what is left of the glide, which lands
                // on a critically damped spring (a notch in about 150 ms, near
                // Chromium's 175 ms smooth scroll), at most 12,000 px/s so a
                // long spin never jumps. A wheel turn is a scroll gesture
                // (scrolling), stops a flick and stops at the ends; at once
                // with reduced motion. A touchpad drag or flick lets go of it.
                function wheelBy(pixels) {
                    if (moving)
                        cancelFlick()
                    pad.held = false
                    const at = contentY - originY, max = contentHeight - height
                    wheelClock.left = Math.max(-at, Math.min(max - at, wheelClock.left + pixels))
                    if (Math.abs(wheelClock.left) >= 0.5 && !wheelClock.running) {
                        wheelClock.speed = 0
                        wheelClock.start()
                    }
                }
                onMovementStarted: wheelClock.stop()
                // A touchpad gesture moves the view with the fingers as it
                // goes and, let go, flicks on at their speed (Flickable's
                // momentum), as Chromium's fling does.
                QtObject {
                    id: pad
                    property bool held: false
                    property real speed: 0
                    property real stamp: 0
                }
                function trackBy(pixels, stamp) {
                    wheelClock.stop()
                    if (moving)
                        cancelFlick()
                    const dt = (stamp - pad.stamp) / 1000
                    pad.speed = pad.held && dt > 0 && dt < 0.1 ? 0.6 * pixels / dt + 0.4 * pad.speed : 0
                    pad.stamp = stamp
                    pad.held = true
                    const at = contentY - originY, max = contentHeight - height
                    contentY += Math.max(-at, Math.min(max - at, pixels))
                    padRest.restart()
                }
                function release() {
                    padRest.stop()
                    if (!pad.held)
                        return
                    const speed = pad.speed
                    pad.held = false
                    pad.speed = 0
                    if (Math.abs(speed) > 50 && !Theme.reducedMotion)
                        flick(0, -speed)
                }
                // A touchpad that never reports its gesture's end.
                Timer {
                    id: padRest
                    interval: 150
                    onTriggered: {
                        pad.speed = 0
                        transcript.release()
                    }
                }
                WheelScroll {
                    target: transcript
                    onNotched: pixels => transcript.wheelBy(pixels)
                    onTracked: (pixels, stamp) => transcript.trackBy(pixels, stamp)
                    onReleased: transcript.release()
                    onWheeled: deltaY => scrollBar.wheeled(deltaY)
                }
                FrameAnimation {
                    id: wheelClock
                    objectName: "wheelClock"
                    property real left
                    property real speed
                    onRunningChanged: if (!running) left = speed = 0
                    onTriggered: {
                        const at = transcript.contentY - transcript.originY
                        const max = transcript.contentHeight - transcript.height
                        left = Math.max(-at, Math.min(max - at, left))
                        let step = left
                        if (!Theme.reducedMotion) {
                            const dt = Math.min(Math.max(frameTime, 0), 0.05)
                            const steps = Math.max(1, Math.ceil(dt / 0.004)), h = dt / steps, w = 52
                            step = 0
                            for (let i = 0; i < steps; ++i) {
                                speed += (w * w * (left - step) - 2 * w * speed) * h
                                speed = Math.max(-12000, Math.min(12000, speed))
                                step += speed * h
                            }
                            if (Math.abs(left - step) < 0.5 && Math.abs(speed) < 8)
                                step = left
                        }
                        step = Math.max(-at, Math.min(max - at, step))
                        left -= step
                        transcript.contentY += step
                        if (left === 0)
                            stop()
                    }
                }
                // Jump to latest (chat.js scrollToBottom()): follow again and
                // glide to the end in 420 ms plus 0.05 ms a pixel (at most
                // 950 ms) on an ease-out quart, aimed at the end each frame;
                // at once with reduced motion or under 2 px. It moves through
                // the conversation as the scroll bar measures it (seek()), so
                // ListView's estimate of the rows ahead does not lurch it. A
                // scroll gesture lets go of it.
                readonly property bool jumping: jumpClock.running
                function jump() {
                    follow = true
                    const from = offset(), gap = fullHeight - height - from
                    if (Theme.reducedMotion || gap < 2)
                        return stick()
                    jumpClock.from = from
                    jumpClock.length = Math.min(950, 420 + gap * 0.05)
                    jumpClock.restart()
                }
                FrameAnimation {
                    id: jumpClock
                    property real from
                    property real length: 1
                    onTriggered: {
                        if (!transcript.follow || transcript.scrolling || Theme.reducedMotion) {
                            stop()
                            return transcript.stick()
                        }
                        const p = Math.min(1, elapsedTime * 1000 / length)
                        const end = transcript.fullHeight - transcript.height
                        if (p < 1) {
                            transcript.seek(from + (end - from) * (1 - Math.pow(1 - p, 4)))
                        } else {
                            stop()
                            transcript.stick()
                        }
                    }
                }
                // A streamed row dropped `height` px of text `top` px into it
                // (negative: something above that point grew by it).
                function dropped(row, top, height) {
                    if (row.key !== anchorKey || anchorOffset <= top)
                        return
                    anchorOffset -= Math.min(height, anchorOffset - top)
                    Qt.callLater(keep)
                }
                onContentYChanged: {
                    if (scrolling) {
                        follow = atEnd()
                        mark()
                    }
                    place()
                }
                // Kept during layout, before a jump is drawn. Stick is deferred:
                // a scroll can change contentHeight before contentY.
                onContentHeightChanged: {
                    keep()
                    Qt.callLater(glide)
                    Qt.callLater(measureAll)
                }
                onCountChanged: Qt.callLater(glide)
                onHeightChanged: {
                    Qt.callLater(stick)
                    Qt.callLater(measureAll)
                }
                // The scroll bar measures the conversation as the browser
                // does, whole: each row's height as last laid out, by index,
                // where ListView takes the rows it has released (beyond the
                // cache) for the average of those it holds.
                property var heights: []
                property real fullHeight: height
                function noteHeight(index, rowHeight) {
                    if (heights[index] === rowHeight)
                        return
                    heights[index] = rowHeight
                    Qt.callLater(measureAll)
                }
                function rowHeight(index, average) {
                    const known = heights[index]
                    return known === undefined ? average : known
                }
                function averageHeight() {
                    let sum = 0, known = 0
                    for (let i = 0; i < count; ++i) {
                        if (heights[i] !== undefined) {
                            sum += heights[i]
                            known++
                        }
                    }
                    return known ? sum / known : 0
                }
                function measureAll() {
                    const average = averageHeight()
                    let total = (headerItem ? headerItem.height : 0) + (footerItem ? footerItem.height : 0)
                    for (let i = 0; i < count; ++i)
                        total += rowHeight(i, average)
                    fullHeight = Math.max(height, total)
                }
                // How far the view's top is into the whole conversation.
                function offset() {
                    const row = itemAt(0, contentY)
                    const head = headerItem ? headerItem.height : 0
                    if (!row || row.index === undefined)
                        return Math.max(0, contentY - originY)
                    const average = averageHeight()
                    let above = head
                    for (let i = 0; i < row.index; ++i)
                        above += rowHeight(i, average)
                    return above + contentY - row.y
                }
                // A drag on the scroll bar: to that offset into the conversation.
                function seek(target) {
                    const head = headerItem ? headerItem.height : 0
                    const end = fullHeight - height
                    if (target >= end - 0.5)
                        return positionViewAtEnd()
                    if (target < head) {
                        positionViewAtBeginning()
                        contentY = originY + target
                        return
                    }
                    const average = averageHeight()
                    let above = head
                    for (let i = 0; i < count; ++i) {
                        const rowSize = rowHeight(i, average)
                        if (above + rowSize > target || i === count - 1) {
                            positionViewAtIndex(i, ListView.Beginning)
                            contentY += target - above
                            return
                        }
                        above += rowSize
                    }
                }
                property real viewOffset: 0
                function place() { viewOffset = offset() }
                onFullHeightChanged: Qt.callLater(place)
                Connections {
                    target: transcript.model
                    function onRowsInserted(parent, first, last) {
                        transcript.heights.splice(first, 0, ...new Array(last - first + 1))
                        Qt.callLater(transcript.measureAll)
                    }
                    // A row that leaves takes the selection with it (Selection.model).
                    function onRowsRemoved(parent, first, last) {
                        transcript.heights.splice(first, last - first + 1)
                        Qt.callLater(transcript.measureAll)
                    }
                    function onRowsMoved() { transcript.heights = []; Qt.callLater(transcript.measureAll) }
                    function onModelReset() {
                        transcript.heights = []
                        Qt.callLater(transcript.measureAll)
                        transcript.opening = true
                        opened.restart()
                    }
                }
                // Never shown (the bar is scrollBar, below): attached, it keeps
                // ListView's reach in step as it always has (positioning at the
                // start from far below otherwise stops under the header).
                ScrollBar.vertical: ScrollBar {
                    visible: false
                    interactive: false
                }
                delegate: ChatEntry {
                    onHeightChanged: ListView.view.noteHeight(index, height)
                    Component.onCompleted: ListView.view.noteHeight(index, height)
                }
                // .thread-list's 56 px top padding.
                header: Item { height: 56 }
                // The live run's metrics, then its working Ghost: after the
                // rows, never a row, so growing rows above stay built. Then
                // .thread-list's bottom padding: 32 px above the composer
                // and whatever notice stands over it.
                footer: Column {
                    width: transcript.width
                    bottomPadding: 32 + composerFrame.space + notices.room
                    readonly property alias slot: workingGhost
                    // The operations waiting for an answer, at the end of
                    // the reply as OpenGhost appends them to its message
                    // (ApprovalCard); each folds away once OpenGhost resolves it.
                    Column {
                        objectName: "approvals"
                        x: (transcript.width - width) / 2
                        width: Math.min(680, transcript.width - 48)
                        Repeater {
                            model: approvalModel
                            ApprovalCard {
                                required property bool leaving
                                dismissed: leaving
                                frontend: window.frontend
                                detailsOpen: window.approvalDetails
                                onDetailsToggled: open => window.approvalDetails = open
                                onAnswer: allow => window.frontend.approve(approval.requestId, allow)
                                onGone: window.dropApproval(approval.requestId)
                            }
                        }
                    }
                    Metrics {
                        objectName: "liveMetrics"
                        readonly property var run: window.frontend.liveMetrics
                        // Its footer's y and the view's contentY are both
                        // content coordinates.
                        seen: exposure.exposed &&
                              parent.y + y < transcript.contentY + transcript.height &&
                              parent.y + y + height > transcript.contentY
                        x: (transcript.width - width) / 2
                        width: Math.min(680, transcript.width - 48)
                        gap: 8
                        visible: run.text !== undefined
                        template: run.text ?? ""
                        tip: run.tip ?? ""
                        started: run.started ?? -1
                        live: run.live ?? false
                    }
                    WorkingGhost {
                        id: workingGhost
                        width: transcript.width
                        columnX: (transcript.width - Math.min(680, transcript.width - 48)) / 2
                        // An approval stands in its place (chat.js approve()).
                        working: window.working && !window.approving
                        run: window.runs
                    }
                }
            }
            // A selection dragged across the rows, and its autoscroll.
            SelectDriver {
                view: transcript
                scrollPadding: transcript.scrollPadding
                frontend: window.frontend
            }
            SelectionVeil {
                objectName: "selectionVeil"
                anchors.fill: parent
                feed: transcript
                progress: window.veilProgress
            }
            SelectionMenu {
                id: selectionToolbar
                view: transcript
                onAsk: text => window.quote(text)
            }
            // .thread-scrollbar: the panel's height, 2 px from its right
            // edge, over the top fade and under the bottom one (1.2's
            // .scrollbar rule sets its top and bottom to 0).
            OverlayScrollBar {
                id: scrollBar
                objectName: "scrollBar"
                view: transcript
                watchesWheel: false
                orientation: Qt.Vertical
                x: transcriptArea.width - width - 2
                y: 0
                z: 1
                height: transcriptArea.height
                size: transcript.height / transcript.fullHeight
                position: Math.max(0, Math.min(1 - size, transcript.viewOffset / transcript.fullHeight))
                onPositionChanged: if (pressed) {
                    const target = position * transcript.fullHeight
                    if (Math.abs(target - transcript.offset()) >= 0.5)
                        transcript.seek(target)
                }
            }
            // An empty conversation's welcome Ghost, which flies to the
            // working Ghost when the first message goes. The splash's
            // Ghost lands on it. It stands --welcome-space above the
            // centred composer (.welcome's top), and the stylesheet hides
            // it in a short window (≤ 540 px).
            WelcomeGhost {
                id: welcomeGhost
                objectName: "welcome"
                x: (transcriptArea.width - width) / 2
                y: composerFrame.centredY - composerFrame.welcomeSpace
                shown: transcript.count === 0 && window.height > 540
                held: splashLoader.active
                slot: transcript.footerItem ? transcript.footerItem.slot : null
            }
            // .thread-view's edges: the conversation fades out over 28 px
            // (--composer-bottom-gap + 12) at the top and at the panel's
            // foot, under the composer, where it covers the contour.
            Rectangle {
                width: parent.width
                height: 28
                radius: panel.radius
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.chatBg }
                    GradientStop { position: 0.7; color: Theme.alpha(Theme.chatBg, 0) }
                }
            }
            Rectangle {
                y: parent.height - height
                z: 2
                width: parent.width
                height: 28
                radius: panel.radius
                antialiasing: true
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.alpha(Theme.chatBg, 0) }
                    GradientStop { position: 0.7; color: Theme.chatBg }
                }
            }
            // .thread-bottom: centred 14 px above the composer (and any
            // notice over it), once the reader has left the end by 120 px.
            JumpButton {
                id: jumpLatest
                objectName: "latest"
                x: (parent.width - width) / 2
                y: notices.y - height - 14
                z: 3
                backdrop: transcript
                shown: !transcript.follow
                       && transcript.contentHeight - (transcript.contentY - transcript.originY) - transcript.height > 120
                onClicked: transcript.jump()
            }
            VeilFilter {
                objectName: "jumpVeil"
                source: jumpLatest
                margin: 24
                progress: window.jumpVeil
            }
        }
        // A local refusal, then a status OpenGhost needs seen (routine
        // progress is not shown: OpenGhost has no status line), in
        // .message-error's type, over the composer.
        Column {
            id: notices
            // The height it takes above the composer, gap included.
            readonly property real room: height > 0 ? height + 8 : 0
            x: composerFrame.x
            y: composerFrame.y - room
            z: 2
            width: composerFrame.width
            spacing: 8
            Label {
                objectName: "notice"
                width: parent.width
                visible: text.length > 0
                text: window.notice || (window.settings.noticeError ? window.settings.notice : "")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.danger
                font.pixelSize: 14
            }
            Label {
                objectName: "status"
                width: parent.width
                visible: text.length > 0
                text: window.frontend.routine ? "" : window.frontend.status
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.danger
                font.pixelSize: 14
            }
            Button {
                objectName: "reconcileTurn"
                visible: window.frontend.canRetry && !window.frontend.retryRow
                text: "Retry"
                onClicked: window.frontend.retry()
                padding: 8
                horizontalPadding: 14
                background: Rectangle {
                    radius: 16
                    color: parent.hovered ? Theme.hover : Theme.composerBg
                    border.color: Theme.composerBorder
                }
                contentItem: Label { text: parent.text; color: Theme.text; font.pixelSize: 14 }
            }
        }
        // The composer (.composer): one rounded panel for chosen files, text
        // and controls, at most 680 px and 24 px inside the panel's sides.
        // An empty conversation centres it under the welcome Ghost; the
        // first message glides it to 16 px above the panel's foot.
        Item {
            id: composerFrame
            objectName: "composerFrame"
            z: 2
            width: Math.min(680, panel.width - 48)
            height: 13 + filesHeight + fieldHeight + 10 + 34 + 10
            x: (panel.width - width) / 2
            y: centredY + (dockedY - centredY) * docked
            // --composer-space: its height and its bottom gap.
            readonly property real space: height + 16
            // --welcome-space: the Ghost (96 × 105), 16 px and the folder
            // pill's 44 px; only the pill's in a short window.
            readonly property real welcomeSpace: window.height > 540 ? 96 * 70 / 64 + 16 + 44 : 44
            // .main's flex column: equal room above and below the composer
            // and its welcome space, or its bottom gap once docked.
            readonly property real centredY: welcomeSpace + (panel.height - height - welcomeSpace) / 2
            readonly property real dockedY: panel.height - 16 - height
            property real docked: transcript.count > 0 ? 1 : 0
            Behavior on docked {
                enabled: !Theme.reducedMotion
                NumberAnimation {
                    duration: 600
                    easing.type: Easing.Bezier
                    easing.bezierCurve: Theme.motion
                }
            }
            // .composer-attachments: 4 px above the tiles, 14 px below,
            // less the 10 px gap it takes back.
            readonly property real filesHeight: composerFiles.count > 0 ? 74 : 0
            // .composer-field follows its one-line (24 px) to 40vh textarea
            // on SmoothHeight's spring (260/32), at once with reduced motion.
            readonly property real fieldGoal: Math.max(24, Math.min(composer.implicitHeight,
                                                                    window.height * 0.4))
            property real fieldHeight: 24
            Component.onCompleted: fieldHeight = fieldGoal
            onFieldGoalChanged: {
                if (Theme.reducedMotion || !visible) {
                    grow.stop()
                    grow.speed = 0
                    fieldHeight = grow.at = fieldGoal
                } else if (!grow.running) {
                    grow.at = fieldHeight
                    grow.start()
                }
            }
            FrameAnimation {
                id: grow
                property real at: 24
                property real speed: 0
                onTriggered: {
                    const dt = Math.min(frameTime, 0.032)
                    const steps = Math.max(1, Math.ceil(dt / 0.008)), h = dt / steps
                    for (let n = 0; n < steps; ++n) {
                        speed += ((composerFrame.fieldGoal - at) * 260 - speed * 32) * h
                        at += speed * h
                    }
                    if (Math.abs(composerFrame.fieldGoal - at) < 0.1 && Math.abs(speed) < 0.1) {
                        stop()
                        speed = 0
                        at = composerFrame.fieldGoal
                    }
                    // Whole pixels, as SmoothHeight renders at DPR 1.
                    composerFrame.fieldHeight = Math.round(at)
                }
            }

            // 0 14px 40px rgba(0, 0, 0, .35), then the 1 px rgb(34) inset.
            BoxShadow {
                anchors.fill: parent
                radius: 24
                blur: 40
                offsetY: 14
                color: Theme.alpha("black", 0.35 * Theme.shadow)
            }
            Rectangle {
                anchors.fill: parent
                radius: 24
                antialiasing: true
                color: Theme.composerBg
                border.width: 1
                border.color: Theme.composerBorder
            }
            // A press on the composer's own padding or toolbar edits.
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.IBeamCursor
                onPressed: composer.forceActiveFocus()
            }
            // The chosen files (.attachments-row): 56 px tiles, 10 px apart,
            // scrolled sideways and faded at a clipped edge.
            Flickable {
                id: filesRow
                objectName: "composerCards"
                y: 13
                width: parent.width
                height: composerFrame.filesHeight
                visible: composerFiles.count > 0
                clip: true
                contentWidth: tiles.width + 28
                contentHeight: height
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                Row {
                    id: tiles
                    x: 14
                    y: 4
                    spacing: 10
                    Repeater {
                        model: composerFiles
                        delegate: Item {
                            id: card
                            objectName: "attachmentCard"
                            required property int index
                            required property double token
                            required property string name
                            required property double size
                            // Bound to the uploads map, which changes only with a file's state.
                            readonly property string upload: window.frontend.uploads[String(token)] ?? ""
                            // .attachment.is-file: icon, 10 px, text, 14 px; ≤ 232 px.
                            width: Math.min(232, 50 + Math.max(cardName.implicitWidth,
                                                               cardMeta.implicitWidth) + 14)
                            height: 56
                            HoverHandler { id: cardHover }
                            // White .045 with the .06 inset over it.
                            Rectangle {
                                anchors.fill: parent
                                radius: 12
                                antialiasing: true
                                color: Theme.alpha(Theme.strong, 0.045)
                                border.width: 1
                                border.color: Theme.alpha(Theme.strong, 1 - 0.955 * 0.94)
                            }
                            FileIcon {
                                id: kind
                                x: 10
                                y: 9
                                width: 30
                                height: 38
                                fileName: card.name
                            }
                            Label {
                                id: cardName
                                objectName: "attachmentName"
                                x: 50
                                y: 11
                                width: card.width - 64
                                height: 18
                                verticalAlignment: Text.AlignVCenter
                                text: card.name
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                color: Theme.text
                                font.pointSize: Theme.points(13.5)
                                font.weight: Theme.weight(600)
                            }
                            Label {
                                id: cardMeta
                                objectName: "attachmentMeta"
                                x: 50
                                y: 29
                                width: card.width - 64
                                height: 16
                                verticalAlignment: Text.AlignVCenter
                                text: kind.kind + " · " + window.formatSize(card.size) + ({
                                    waiting: " · waiting",
                                    uploading: " · uploading…",
                                    uploaded: " · uploaded",
                                    failed: " · failed"
                                }[card.upload] ?? "")
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                color: card.upload === "failed" ? Theme.danger : Theme.secondary
                                font.pixelSize: 12
                                font.weight: Theme.weight(500)
                            }
                            // .attachment-remove: an 18 px ✕ that pops in over
                            // the tile's corner on hover or keyboard focus.
                            AbstractButton {
                                id: remove
                                objectName: "removeAttachment"
                                x: card.width - 14
                                y: -4
                                width: 18
                                height: 18
                                hoverEnabled: true
                                // A file being sent stays until OpenGhost answers.
                                enabled: !window.frontend.admitting && card.upload !== "waiting"
                                         && card.upload !== "uploading" && card.upload !== "uploaded"
                                onClicked: window.removeFile(card.index)
                                // Its name, never shown: constant, as the file's name is untrusted.
                                text: "Remove attachment"
                                readonly property bool shown: cardHover.hovered || visualFocus
                                opacity: shown ? 1 : 0
                                scale: shown ? 1 : 0.5
                                Behavior on opacity {
                                    enabled: !Theme.reducedMotion
                                    NumberAnimation { duration: 160; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
                                }
                                Behavior on scale {
                                    enabled: !Theme.reducedMotion
                                    NumberAnimation {
                                        duration: 320
                                        easing.type: Easing.Bezier
                                        easing.bezierCurve: [0.34, 1.56, 0.64, 1, 1, 1]
                                    }
                                }
                                background: Item {
                                    BoxShadow {
                                        anchors.fill: parent
                                        radius: 9
                                        blur: 6
                                        offsetY: 2
                                        color: Theme.alpha("black", 0.35 * Theme.shadow)
                                    }
                                    // The 1.5 px composer-coloured ring.
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: remove.visualFocus ? -3.5 : -1.5
                                        radius: width / 2
                                        antialiasing: true
                                        color: remove.visualFocus ? Theme.alpha(Theme.strong, 0.35)
                                                                  : Theme.composerBg
                                        Rectangle {
                                            anchors.fill: parent
                                            anchors.margins: remove.visualFocus ? 2 : 0
                                            radius: width / 2
                                            antialiasing: true
                                            color: Theme.composerBg
                                        }
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: width / 2
                                        antialiasing: true
                                        color: remove.hovered ? Theme.noteHoverBg : Theme.noteBg
                                    }
                                }
                                contentItem: Item {
                                    PathIcon {
                                        anchors.centerIn: parent
                                        width: 9
                                        height: 9
                                        name: "remove"
                                        color: Theme.alpha(Theme.strong, 0.92)
                                    }
                                }
                            }
                        }
                    }
                }
            }
            Rectangle {
                x: 1
                y: filesRow.y
                width: 28
                height: filesRow.height
                visible: filesRow.visible && filesRow.contentX > 0.5
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: Theme.composerBg }
                    GradientStop { position: 1; color: Theme.alpha(Theme.composerBg, 0) }
                }
            }
            Rectangle {
                x: parent.width - width - 1
                y: filesRow.y
                width: 28
                height: filesRow.height
                visible: filesRow.visible
                         && filesRow.contentX < filesRow.contentWidth - filesRow.width - 0.5
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: Theme.alpha(Theme.composerBg, 0) }
                    GradientStop { position: 1; color: Theme.composerBg }
                }
            }
            // .composer-field: the text, 14 px inside the sides (the
            // textarea's own 6 px more), 16 px on a 24 px line.
            Flickable {
                id: composerScroll
                x: 14
                y: 13 + composerFrame.filesHeight
                width: parent.width - 28
                height: composerFrame.fieldHeight
                clip: true
                contentWidth: width
                contentHeight: composer.height
                boundsBehavior: Flickable.StopAtBounds
                // .composer-scrollbar: the field's height, 11 px past its right edge.
                ScrollBar.vertical: OverlayScrollBar {
                    view: composerScroll
                    parent: composerScroll.parent
                    x: composerScroll.x + composerScroll.width + 11 - width
                    y: composerScroll.y
                    z: 2
                    height: composerScroll.height
                }
                // The caret stays in view while the text is taller than 40vh.
                function reveal(r) {
                    if (contentY > r.y)
                        contentY = r.y
                    else if (contentY + height < r.y + r.height)
                        contentY = r.y + r.height - height
                }
                // "Ask anything" in the composer's grey, until anything is typed.
                Label {
                    x: 6
                    width: parent.width - 12
                    height: 24
                    verticalAlignment: Text.AlignVCenter
                    visible: composer.length === 0 && composer.preeditText.length === 0
                    text: "Ask anything"
                    elide: Text.ElideRight
                    color: Theme.muted
                    font.pixelSize: 16
                }
                TextArea {
                    id: composer
                    objectName: "composer"
                    width: composerScroll.width
                    onCursorRectangleChanged: composerScroll.reveal(cursorRectangle)
                    background: null
                    font.pixelSize: 16
                    color: Theme.text
                    selectionColor: Theme.alpha(Theme.selection, 0.3 / Theme.selection.a)
                    selectedTextColor: Theme.text
                    leftPadding: 6
                    rightPadding: 6
                    // A 24 px CSS line: its room split above and below.
                    readonly property real halfLeading: Theme.halfLeading(font, 24)
                    topPadding: -halfLeading
                    bottomPadding: halfLeading
                    Accessible.name: "Message"
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    persistentSelection: true
                    // Again once the change is complete: setting `text`
                    // replaces the document after this signal.
                    function relines() { Theme.fieldLines(textDocument, 24) }
                    Component.onCompleted: relines()
                    onTextChanged: {
                        relines()
                        Qt.callLater(relines)
                        if (window.pendingDraft && !text.startsWith(window.pendingDraft))
                            window.pendingIntact = false
                        welcomeGhost.typing()
                    }
                    // Plain Enter sends; Escape stops a running reply, as in OpenGhost.
                    Keys.onPressed: function(event) {
                        if (event.matches(StandardKey.Paste) && window.frontend.pasteRefused()) {
                            event.accepted = true
                            window.notice = window.fileRefusal
                        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                            && (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
                            && !inputMethodComposing) {
                            event.accepted = true
                            window.submit()
                        } else if (event.key === Qt.Key_Escape && window.frontend.canCancel
                                   && !inputMethodComposing && !settingsDialog.visible) {
                            event.accepted = true
                            window.frontend.cancel()
                        }
                    }
                    // A right click on the focused composer pastes text; files
                    // and images are refused, not inserted.
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: {
                            if (!composer.activeFocus)
                                return
                            if (window.frontend.pasteRefused())
                                window.notice = window.fileRefusal
                            else
                                composer.paste()
                        }
                    }
                }
            }
            // .composer-toolbar: add/mode on the left; model, effort and send on the right.
            Item {
                x: 14
                y: 13 + composerFrame.filesHeight + composerFrame.fieldHeight + 10
                width: parent.width - 28
                height: 34
                IconButton {
                    objectName: "attach"
                    glyph: "add"
                    turn: 90
                    hoverK: 170
                    hoverC: 22
                    iconColor: Theme.muted
                    hoverColor: Theme.accent
                    restOpacity: 1
                    hoverOpacity: 1
                    disabledOpacity: 1
                    enabled: !window.frontend.picking && composerFiles.count < 20
                    onClicked: filePicker.choose()
                    text: "Add photos and files"
                    ButtonTip { text: parent.text }
                }
                // .composer-mode, 4 px right of Add: the agent mode, shown
                // once OpenGhost reports its modes.
                ModePicker {
                    id: modePicker
                    x: 34 + 4
                    modes: window.frontend.modes
                    dock: modeDock
                }
                Row {
                    anchors.right: parent.right
                    spacing: 4
                    // .composer-model: the sparkle; it opens the model stage.
                    ModelButton {
                        id: modelChoice
                        objectName: "modelChoice"
                        displayText: window.settings.modelLabel
                        // A key presses it as a click does (Space, or Enter as
                        // Space: ButtonKeys) and opens the stage on release,
                        // with the keyboard's focus.
                        property bool byKey: false
                        onClicked: {
                            if (window.settings.choices.length) {
                                modelStage.openStage(byKey)
                            } else {
                                settingsDialog.open()
                                settingsDialog.show("providers", true)
                            }
                            byKey = false
                        }
                        onCanceled: byKey = false
                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Space && !event.isAutoRepeat) {
                                byKey = true
                                return // The button presses.
                            }
                            // Up/Down/Home/End step through the listed provider's models.
                            const ids = window.settings.modelIds, at = ids.indexOf(window.settings.model)
                            const to = event.key === Qt.Key_Home ? 0 : event.key === Qt.Key_End ? ids.length - 1
                                : event.key === Qt.Key_Up ? Math.max(0, at - 1)
                                : event.key === Qt.Key_Down ? Math.min(ids.length - 1, at + 1) : -1
                            if (to >= 0 && ids.length) {
                                event.accepted = true
                                // At the list's edge the key changes nothing.
                                if (to !== at)
                                    window.settings.chooseModel(ids[to])
                            }
                        }
                    }
                    EffortControl {
                        id: thinkingChoice
                        objectName: "thinkingChoice"
                        locked: window.frontend.busy
                        settings: window.settings
                        backdrop: window.contentItem
                    }
                    // .composer-send: the white round button, dark arrow; it
                    // looks the same when there is nothing to send, and a
                    // send during a run steers it.
                    IconButton {
                        objectName: "send"
                        glyph: "send"
                        iconColor: Theme.onAccent
                        restOpacity: 1
                        hoverOpacity: 1
                        disabledOpacity: 1
                        fill: Theme.accent
                        radius: 17
                        ringOutset: 4
                        lift: 6
                        pressScale: 0.06
                        enabled: !window.frontend.admitting
                                 && (window.frontend.ready && !window.frontend.busy || window.frontend.canSteer)
                                 && (composer.text.trim().length > 0 || composerFiles.count > 0)
                        onClicked: window.submit()
                        text: "Send"
                    }
                }
            }
        }
        // .is-veiled > .composer: the composer steps back with the feed.
        VeilFilter {
            objectName: "composerVeil"
            source: composerFrame
            margin: 72
            progress: window.composerVeil
        }
    }

    ModeDock {
        id: modeDock
        parent: Overlay.overlay
        button: modePicker
        backdrop: window.contentItem
        onPicked: id => window.frontend.setPermissionMode(id)
    }

    ModelStage {
        id: modelStage
        settings: window.settings
        button: modelChoice
        backdrop: window.contentItem
        composer: composerFrame
        input: composer
    }

    // Dropped files and URLs are never attached, fetched or inserted. A
    // link dragged from the transcript itself (SelectArea) is no file: let
    // go over the window it does nothing, as 1.2's does.
    DropArea {
        objectName: "dropRefusal"
        anchors.fill: parent
        keys: ["text/uri-list"]
        onEntered: drag => drag.accepted = !drag.source
        onDropped: drop => {
            drop.accepted = false
            window.notice = window.fileRefusal
        }
    }

    // The welcome Ghost watches the pointer anywhere in the window.
    HoverHandler {
        target: null
        enabled: welcomeGhost.phase === "shown"
        onPointChanged: welcomeGhost.lookAt(point.scenePosition)
    }
    // OpenGhost 1.3's original splash and app reveal, copied from its native port.
    Loader {
        id: splashLoader
        parent: window.Overlay.overlay
        anchors.fill: parent
        z: 1000
        active: !Theme.reducedMotion
        sourceComponent: Splash {
            objectName: "splash"
            welcome: welcomeGhost
            onFinished: splashLoader.active = false
        }
    }
    Binding {
        when: splashLoader.item !== null && splashLoader.item.revealing
        target: window.contentItem
        property: "opacity"
        value: splashLoader.item ? splashLoader.item.reveal : 1
        restoreMode: Binding.RestoreBindingOrValue
    }
    Binding {
        when: splashLoader.item !== null && splashLoader.item.revealing
        target: window.contentItem
        property: "scale"
        value: splashLoader.item ? 0.975 + 0.025 * splashLoader.item.reveal : 1
        restoreMode: Binding.RestoreBindingOrValue
    }
}
