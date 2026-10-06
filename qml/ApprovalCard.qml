import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import OpenGhost.Cpp

// The card that asks before the agent acts (OpenGhost 1.3 approval-card.js,
// .approval). It leads with what the step is for in plain words, says what
// it does to the computer and where, and keeps the command or the changes
// one click away. Everything it shows is OpenGhost's description of the
// pending operation (WindowController::approvals): the effect, places, command and
// changes come from the backend, never from this view. Allow and Deny send
// the answer once; the card then settles (its buttons fade) and OpenGhost's
// resolution dismisses it. Tool activity uses the working ghost, not a
// separate tool-result panel.
Item {
    id: card
    objectName: "approvalCard"
    // {requestId, card, followUp, answered} (WindowController::approvals).
    required property var approval
    // The window controller: selected text copies through it.
    property var frontend: null
    readonly property var info: approval.card ?? ({})
    readonly property var followUp: approval.followUp ?? null
    // Opening the details once keeps them open on the next cards (the
    // window's choice, not saved).
    property bool detailsOpen: false
    signal detailsToggled(bool open)
    // Allow or Deny was chosen here, before OpenGhost's backend confirms it.
    property bool chosen: false
    readonly property bool settled: chosen || approval.answered === true
    signal answer(bool allow)
    // The asker's own decisions, when it offers more than Allow and Deny (Pi's
    // plugin-permissions: allow for the session, deny with a reason, …), each
    // with its shortcut. The card shows and sends them; the asker applies them.
    readonly property var actions: approval.actions ?? []
    readonly property bool rich: actions.length > 0
    readonly property var scopes: approval.scopes ?? ({})
    readonly property bool doublePress: approval.doublePress === true
    signal decision(string action, string note, string scope)
    // "decision"; "reason" while a deny's reason is typed; "scope" while a
    // subagent's session grant waits for its reach.
    property string step: "decision"
    property string pendingAction: ""
    // A shortcut pressed once, waiting for its second press (double-press).
    property string armed: ""
    function offered(id) {
        for (let i = 0; i < actions.length; ++i)
            if (actions[i].id === id)
                return actions[i]
        return null
    }
    readonly property var verbs: ({
        approve: "allow", approveSession: "allow for this session",
        approveSessionBoth: "allow both directions for this session",
        deny: "deny", denyWithReason: "deny with a reason"
    })
    readonly property var extras: actions.filter(a => a.id === "approveSessionBoth" || a.id === "denyWithReason")
    readonly property bool secondRow: rich && step === "decision" && (extras.length > 0 || doublePress)
    readonly property string hint: {
        if (!armed || !offered(armed))
            return ""
        return "Press " + offered(armed).key + " again to " + verbs[armed] + "."
    }
    // OpenGhost resolved it: the card folds away, then `gone`.
    property bool dismissed: false
    signal gone()
    // .approval's margin-top (none at the top of an empty message).
    property real gap: 14

    // .approval --tone: green only looks, amber changes or installs, red
    // deletes or touches the system, blue goes online, grey runs a program
    // or records history.
    readonly property color tone: {
        switch (info.effect) {
        case "read": return Theme.success
        case "change": case "install": return Theme.warn
        case "delete": case "system": return Theme.danger
        case "online": return Theme.link
        default: return Theme.strong
        }
    }
    readonly property var effectNames: ({
        read: "Only looks", change: "Changes files", delete: "Deletes files",
        install: "Installs software", system: "Changes system settings", online: "Goes online",
        run: "Runs a program", record: "Updates project history"
    })
    readonly property var places: info.places ?? []
    readonly property int maxPlaces: 3
    // The chips under the headline, in order: the effect (commands only),
    // up to three places and how many more, the quote; then the follow-up
    // command's effect when an edit runs one after.
    readonly property var chips: {
        const list = []
        if (info.badge && info.effect)
            list.push({type: "effect", text: effectNames[info.effect] ?? info.effect, tone: tone})
        for (let i = 0; i < Math.min(places.length, maxPlaces); ++i)
            list.push({type: "place", kind: places[i].kind, text: places[i].label, title: places[i].title})
        if (places.length > maxPlaces)
            list.push({type: "count", text: "+" + (places.length - maxPlaces)})
        if (info.quote)
            list.push({type: "quote", text: info.quote})
        if (followUp && followUp.effect)
            list.push({type: "effect", text: effectNames[followUp.effect] ?? followUp.effect,
                       tone: card.toneOf(followUp.effect)})
        return list
    }
    function toneOf(effect) {
        switch (effect) {
        case "read": return Theme.success
        case "change": case "install": return Theme.warn
        case "delete": case "system": return Theme.danger
        case "online": return Theme.link
        default: return Theme.strong
        }
    }
    // The changes: one block for OpenGhost's single edit, one per file for
    // a files edit (which may contain just one file).
    readonly property var diffs: {
        if (info.removed || info.added)
            return [{path: "", removed: info.removed ?? "", added: info.added ?? ""}]
        return info.diffs ?? []
    }
    readonly property string code: info.code ?? ""
    readonly property string followCode: followUp ? followUp.code ?? "" : ""
    readonly property bool hasDetails: code.length > 0 || diffs.length > 0 || followCode.length > 0
    readonly property bool revealable: hasDetails && !!info.reveal
    readonly property bool open: revealable && detailsOpen

    // approval-card.js lines(): the first 14 lines and how many more.
    function lines(text) {
        const all = String(text).replace(/\r\n/g, "\n").replace(/\n$/, "").split("\n")
        return {shown: all.slice(0, 14), more: Math.max(0, all.length - 14)}
    }

    width: parent ? parent.width : 0
    // Folding away takes the margin and padding with it.
    height: gap * (1 - fold.value) + frame.height
    // Folding, the frame takes its height, padding and margin with it (LEAVE).
    Accessible.role: Accessible.Grouping
    Accessible.name: info.title ?? ""

    // approval-in: up 8 px from .98, 0.5 s on the motion curve.
    Entrance { id: enter; duration: 500 }
    // The pieces after it, in order of their --i (approval-word, -pop, -rise).
    readonly property var pop: [0.34, 1.56, 0.64, 1, 1, 1]
    Component.onCompleted: enter.play()

    // The dismissal (approval-card.js LEAVE): 300 ms on the motion curve to
    // no height, .97 and a 4 px blur; at once with reduced motion.
    QtObject {
        id: fold
        property real value: 0
    }
    NumberAnimation {
        id: folding
        target: fold
        property: "value"
        from: 0
        to: 1
        duration: 300
        easing.type: Easing.Bezier
        easing.bezierCurve: Theme.motion
        onFinished: card.gone()
    }
    onDismissedChanged: if (dismissed) {
        if (Theme.reducedMotion) {
            fold.value = 1
            Qt.callLater(() => card.gone()) // Not inside the list's own update.
        } else {
            folding.restart()
        }
    }

    Item {
        id: frame
        objectName: "approvalFrame"
        y: card.gap * (1 - fold.value)
        width: card.width
        // .approval-head: the icon or the main column (5 px down), whichever is taller.
        readonly property real head: Math.max(32, 5 + main.height)
        readonly property real inset: 14 * (1 - fold.value) // padding-top
        readonly property real natural: 14 + head + details.height + 14 + foot.height + 12
        height: natural * (1 - fold.value)
        opacity: enter.value * (1 - fold.value)
        transformOrigin: Item.Center
        scale: (0.98 + 0.02 * enter.value) * (1 - 0.03 * fold.value)
        transform: Translate { y: 8 * (1 - enter.value) }
        layer.enabled: fold.value > 0
        layer.effect: MultiEffect {
            blurEnabled: true
            blurMax: 16
            autoPaddingEnabled: true
            blur: Math.min(1, 4 * fold.value / (0.27 * 16))
        }

        // 0 10px 30px rgba(0, 0, 0, .22), then the 1 px composer border inset.
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
        // What the card holds; folding, it is cut at the card's edge
        // (overflow: hidden), never its shadow.
        Item {
            id: body
            anchors.fill: parent
            clip: fold.value > 0

            // .approval-icon: the kind's glyph on its tone at .12.
            Rectangle {
                objectName: "approvalIcon"
                x: 14
                y: frame.inset
                width: 32
                height: 32
                radius: 10
                antialiasing: true
                color: Theme.alpha(card.tone, 0.12)
                PathIcon {
                    x: 8 // 7.5, which Chromium snaps to 8.
                    y: 8
                    width: 17
                    height: 17
                    name: card.info.kind === "file" ? "file" : card.info.kind === "web" ? "globe" : "terminal"
                    color: card.tone
                }
            }

            // .approval-main: the headline, then the chips, 8 px apart, 5 px down.
            Column {
                id: main
                x: 14 + 32 + 12
                y: frame.inset + 5
                width: frame.width - x - 14
                spacing: 8

                // .approval-title: 15/22 at 600, each word out of a blur in turn
                // (approval-word, 60 ms + 26 ms a word).
                Flow {
                    id: title
                    objectName: "approvalTitle"
                    width: parent.width
                    spacing: words.advanceWidth(" ")
                    FontMetrics {
                        id: words
                        font.pixelSize: 15
                        font.weight: Theme.weight(600)
                    }
                    Repeater {
                        model: String(card.info.title ?? "").split(/\s+/).filter(word => word.length > 0)
                        Label {
                            id: word
                            required property string modelData
                            required property int index
                            width: Math.min(implicitWidth, title.width)
                            height: lineCount * 22
                            text: modelData
                            textFormat: Text.PlainText
                            wrapMode: Text.WrapAnywhere
                            lineHeightMode: Text.FixedHeight
                            lineHeight: 22
                            topPadding: Theme.halfLeading(font, 22)
                            bottomPadding: -topPadding
                            color: Theme.text
                            font: words.font
                            Entrance { id: wordIn; duration: 450; delay: 60 + word.index * 26 }
                            Component.onCompleted: wordIn.play()
                            opacity: wordIn.value
                            transform: Translate { y: 15 * 0.35 * (1 - wordIn.value) }
                            layer.enabled: wordIn.value < 1
                            layer.effect: MultiEffect {
                                blurEnabled: true
                                blurMax: 24
                                autoPaddingEnabled: true
                                blur: Math.min(1, 6 * (1 - wordIn.value) / (0.27 * 24))
                            }
                        }
                    }
                }

                // .approval-meta: the chips, wrapping, 6 px apart.
                Flow {
                    objectName: "approvalMeta"
                    width: parent.width
                    spacing: 6
                    visible: card.chips.length > 0
                    Repeater {
                        model: card.chips
                        Rectangle {
                            id: chip
                            objectName: "approvalChip"
                            required property var modelData
                            required property int index
                            readonly property string type: modelData.type
                            readonly property bool quote: type === "quote"
                            readonly property real inner: (type === "place" ? 14 + 6 : type === "effect" ? 6 + 6 : 0)
                            // .approval-place stops at 240 px; a quote wraps.
                            width: quote ? Math.min(parent.width, chipText.implicitWidth + 20)
                                 : Math.min(type === "place" ? 240 : parent.width,
                                            (type === "place" ? 8 : 10) + inner + chipText.implicitWidth + 10)
                            height: quote ? chipText.height + 4 : 24
                            radius: 12
                            antialiasing: true
                            color: type === "effect" ? Theme.alpha(modelData.tone, 0.12)
                                                     : Theme.alpha(Theme.strong, 0.06)
                            // approval-pop: from .85, 180 ms + 45 ms a chip.
                            Entrance {
                                id: chipIn
                                duration: 450
                                delay: 180 + chip.index * 45
                                curve: card.pop
                            }
                            Component.onCompleted: chipIn.play()
                            opacity: chipIn.value
                            scale: 0.85 + 0.15 * chipIn.value
                            Accessible.name: chipText.text
                            // The effect's 6 px dot in its own colour.
                            Rectangle {
                                visible: chip.type === "effect"
                                x: 10
                                anchors.verticalCenter: parent.verticalCenter
                                width: 6
                                height: 6
                                radius: 3
                                color: chip.modelData.tone ?? Theme.strong
                            }
                            PathIcon {
                                visible: chip.type === "place"
                                x: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: 14
                                height: 14
                                name: chip.modelData.kind === "folder" ? "folder-shut"
                                    : chip.modelData.kind === "site" ? "globe" : "file"
                                color: Theme.secondary
                            }
                            Label {
                                id: chipText
                                x: chip.quote ? 10 : (chip.type === "place" ? 8 : 10) + chip.inner
                                y: chip.quote ? 2 : 0
                                width: chip.quote ? Math.min(implicitWidth, chip.parent.width - 20)
                                                  : chip.width - x - 10
                                height: chip.quote ? lineCount * 20 : 24
                                text: chip.modelData.text ?? ""
                                textFormat: Text.PlainText
                                wrapMode: chip.quote ? Text.WrapAtWordBoundaryOrAnywhere : Text.NoWrap
                                elide: chip.quote ? Text.ElideNone : Text.ElideRight
                                lineHeightMode: Text.FixedHeight
                                lineHeight: chip.quote ? 20 : 24
                                topPadding: Theme.halfLeading(font, lineHeight)
                                bottomPadding: -topPadding
                                color: chip.type === "effect" ? chip.modelData.tone
                                     : chip.type === "count" ? Theme.secondary : Theme.text
                                font.pointSize: Theme.points(12.5)
                                font.weight: Font.Medium // 550: Chromium draws the nearest face
                            }
                            // The place's whole path, on hover (its title).
                            ButtonTip {
                                enabled: chip.type === "place"
                                text: chip.modelData.title ?? ""
                            }
                        }
                    }
                }
            }

            // .approval-details: the command or the changes, unfolding under
            // the headline (420 ms on the motion curve; the inside out of a
            // 4 px blur and 6 px up, or back into the blur in 220 ms).
            Item {
                id: details
                objectName: "approvalDetails"
                x: 14
                y: frame.inset + frame.head
                width: frame.width - 28
                property real reveal: card.open ? 1 : 0
                Behavior on reveal {
                    enabled: !Theme.reducedMotion && card.revealable
                    NumberAnimation {
                        duration: 420
                        easing.type: Easing.Bezier
                        easing.bezierCurve: Theme.motion
                    }
                }
                // The inside's own fade: 420 ms in, 220 ms out.
                property real shown: card.open ? 1 : 0
                Behavior on shown {
                    enabled: !Theme.reducedMotion && card.revealable
                    NumberAnimation {
                        duration: card.open ? 420 : 220
                        easing.type: Easing.Bezier
                        easing.bezierCurve: Theme.motion
                    }
                }
                height: inner.height * details.reveal
                visible: details.reveal > 0
                clip: details.reveal < 1
                Column {
                    id: inner
                    objectName: "approvalDetailsInner"
                    width: parent.width
                    topPadding: 12
                    spacing: 8
                    opacity: details.shown
                    transform: Translate { y: card.open ? -6 * (1 - details.shown) : 0 }
                    layer.enabled: details.visible && details.shown < 1
                    layer.effect: MultiEffect {
                        blurEnabled: true
                        blurMax: 16
                        autoPaddingEnabled: true
                        blur: Math.min(1, 4 * (1 - details.shown) / (0.27 * 16))
                    }
                    // .approval-code
                    CodeWell {
                        visible: card.code.length > 0
                        text: card.code
                    }
                    // .approval-diff, one per changed file.
                    Repeater {
                        model: card.open || details.visible ? card.diffs : []
                        Column {
                            id: change
                            required property var modelData
                            width: inner.width
                            spacing: 4
                            Label {
                                visible: !!change.modelData.path && card.diffs.length > 1
                                width: parent.width
                                text: change.modelData.path ?? ""
                                elide: Text.ElideMiddle
                                color: Theme.secondary
                                font.pixelSize: 12
                                font.weight: Font.Medium // 550: Chromium draws the nearest face
                            }
                            DiffWell {
                                removed: change.modelData.removed ?? ""
                                added: change.modelData.added ?? ""
                            }
                        }
                    }
                    // An edit's follow-up command, after its changes.
                    CodeWell {
                        visible: card.followCode.length > 0
                        text: card.followCode
                    }
                }
            }

            // .approval-foot: the reveal on the left; Deny and Allow on the
            // right, 8 px apart, 14 px under the rest. Each rises in by its --i
            // (approval-rise, 240 ms + 50 ms).
            Item {
                id: foot
                x: 14
                y: frame.inset + frame.head + details.height + 14
                width: frame.width - 28
                height: 30 + (card.secondRow ? 28 : 0)

                FootButton {
                    id: revealButton
                    objectName: "approvalReveal"
                    order: 0
                    visible: card.revealable && card.step === "decision"
                    enabled: !card.settled
                    width: revealLabel.implicitWidth + 5 + 12 + 22
                    Accessible.name: revealLabel.text
                    Accessible.role: Accessible.Button
                    Accessible.checkable: true
                    Accessible.checked: card.open
                    onClicked: card.detailsToggled(!card.detailsOpen)
                    background: Rectangle {
                        radius: 15
                        color: revealButton.hovered && !card.settled ? Theme.alpha(Theme.strong, 0.06) : "transparent"
                        Behavior on color { ColorAnimation { duration: 150 } }
                        FocusRing { target: revealButton; inset: 0 }
                    }
                    contentItem: Item {
                        Label {
                            id: revealLabel
                            objectName: "approvalRevealLabel"
                            x: 12
                            height: 30
                            verticalAlignment: Text.AlignVCenter
                            text: ({
                                command: card.open ? "Hide command" : "Show command",
                                changes: card.open ? "Hide changes" : "Show changes",
                                content: card.open ? "Hide content" : "Show content"
                            })[card.info.reveal] ?? ""
                            color: revealButton.hovered && !card.settled ? Theme.text : Theme.secondary
                            Behavior on color { ColorAnimation { duration: 150 } }
                            font.pixelSize: 13
                            font.weight: Font.Medium // 550: Chromium draws the nearest face
                        }
                        // A 12 px chevron, turned up while open (0.4 s).
                        PathIcon {
                            x: revealLabel.x + revealLabel.width + 5
                            anchors.verticalCenter: parent.verticalCenter
                            width: 12
                            height: 12
                            name: "reveal-chevron"
                            color: revealLabel.color
                            rotation: card.open ? 180 : 0
                            Behavior on rotation {
                                enabled: !Theme.reducedMotion
                                NumberAnimation {
                                    duration: 400
                                    easing.type: Easing.Bezier
                                    easing.bezierCurve: Theme.motion
                                }
                            }
                        }
                    }
                }
                Row {
                    anchors.right: parent.right
                    spacing: 8
                    visible: card.step === "decision"
                    Choice {
                        objectName: "approvalDeny"
                        order: 1
                        text: "Deny"
                        shortcut: card.offered("deny")?.key ?? ""
                        armedHere: card.armed === "deny"
                        fill: hovered && !card.settled ? Theme.alpha(Theme.strong, 0.11)
                                                       : Theme.alpha(Theme.strong, 0.07)
                        ink: Theme.text
                        onClicked: card.choose(false)
                    }
                    Choice {
                        objectName: "approvalAllowSession"
                        order: 2
                        visible: !!card.offered("approveSession")
                        text: "Allow for session"
                        shortcut: card.offered("approveSession")?.key ?? ""
                        armedHere: card.armed === "approveSession"
                        tip: card.offered("approveSession")?.detail ?? ""
                        fill: hovered && !card.settled ? Theme.alpha(Theme.accent, 0.22)
                                                       : Theme.alpha(Theme.accent, 0.14)
                        ink: Theme.text
                        onClicked: card.pick("approveSession")
                    }
                    Choice {
                        objectName: "approvalAllow"
                        order: 3
                        text: "Allow"
                        shortcut: card.offered("approve")?.key ?? ""
                        armedHere: card.armed === "approve"
                        fill: Theme.accent
                        ink: Theme.onAccent
                        onClicked: card.choose(true)
                    }
                }
                // The armed shortcut's hint; the rarer decisions as quieter links.
                Label {
                    objectName: "approvalHint"
                    visible: card.secondRow
                    y: 36
                    height: 22
                    width: parent.width - extraRow.width - 12
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    text: card.hint
                    color: Theme.secondary
                    font.pixelSize: 12
                }
                Row {
                    id: extraRow
                    visible: card.secondRow
                    anchors.right: parent.right
                    y: 36
                    spacing: 4
                    Repeater {
                        model: card.extras
                        Link {
                            required property var modelData
                            objectName: modelData.id === "denyWithReason" ? "approvalDenyReason"
                                                                          : "approvalAllowBoth"
                            text: modelData.id === "denyWithReason" ? "Deny with reason"
                                                                    : "Allow both for session"
                            shortcut: modelData.key
                            armedHere: card.armed === modelData.id
                            tip: modelData.detail ?? ""
                            onClicked: card.pick(modelData.id)
                        }
                    }
                }

                // Deny with a reason: what the agent is told, sent with the Deny.
                TextField {
                    id: reasonField
                    objectName: "approvalReason"
                    visible: card.step === "reason"
                    enabled: !card.settled
                    width: parent.width - reasonButtons.width - 8
                    height: 30
                    leftPadding: 12
                    rightPadding: 12
                    placeholderText: "Why? The agent is told."
                    color: Theme.text
                    placeholderTextColor: Theme.tertiary
                    font.pixelSize: 13
                    background: Rectangle {
                        radius: 15
                        color: Theme.wellBg
                        border.width: 1
                        border.color: reasonField.activeFocus ? Theme.alpha(Theme.strong, 0.35)
                                                              : Theme.alpha(Theme.strong, 0.08)
                    }
                    Keys.onReturnPressed: card.sendReason()
                    Keys.onEnterPressed: card.sendReason()
                    Keys.onEscapePressed: card.back()
                }
                Row {
                    id: reasonButtons
                    anchors.right: parent.right
                    spacing: 8
                    visible: card.step === "reason"
                    Choice {
                        objectName: "approvalReasonCancel"
                        text: "Cancel"
                        fill: hovered && !card.settled ? Theme.alpha(Theme.strong, 0.11)
                                                       : Theme.alpha(Theme.strong, 0.07)
                        ink: Theme.text
                        onClicked: card.back()
                    }
                    Choice {
                        objectName: "approvalReasonSend"
                        text: "Deny"
                        enabled: !card.settled && reasonField.text.trim().length > 0
                        fill: Theme.alpha(Theme.danger, enabled ? 0.9 : 0.4)
                        ink: Theme.onAccent
                        onClicked: card.sendReason()
                    }
                }

                // A subagent's session grant: for that subagent, or the whole session.
                Label {
                    visible: card.step === "scope"
                    height: 30
                    width: parent.width - scopeButtons.width - 8
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    text: "Allow for"
                    color: Theme.secondary
                    font.pixelSize: 13
                }
                Row {
                    id: scopeButtons
                    anchors.right: parent.right
                    spacing: 8
                    visible: card.step === "scope"
                    Choice {
                        objectName: "approvalScopeCancel"
                        text: "Cancel"
                        fill: hovered && !card.settled ? Theme.alpha(Theme.strong, 0.11)
                                                       : Theme.alpha(Theme.strong, 0.07)
                        ink: Theme.text
                        onClicked: card.back()
                    }
                    Choice {
                        objectName: "approvalScopeSubagent"
                        text: "This subagent"
                        tip: card.scopes.subagent ?? ""
                        fill: hovered && !card.settled ? Theme.alpha(Theme.accent, 0.22)
                                                       : Theme.alpha(Theme.accent, 0.14)
                        ink: Theme.text
                        onClicked: card.commit(card.pendingAction, "", "subagent")
                    }
                    Choice {
                        objectName: "approvalScopeSession"
                        text: "Whole session"
                        tip: card.scopes.session ?? ""
                        fill: Theme.accent
                        ink: Theme.onAccent
                        onClicked: card.commit(card.pendingAction, "", "session")
                    }
                }
            }
        }
    }

    // Once: the buttons fade and nothing more can be chosen here.
    function choose(allow) {
        if (settled)
            return
        armed = ""
        chosen = true
        answer(allow)
    }
    // An offered decision: some open a step first (a reason, a subagent's reach).
    function pick(id) {
        if (settled || !offered(id))
            return
        armed = ""
        if (id === "denyWithReason") {
            step = "reason"
            reasonField.forceActiveFocus()
        } else if ((id === "approveSession" || id === "approveSessionBoth") && card.scopes.subagent) {
            pendingAction = id
            step = "scope"
        } else {
            commit(id, "", "")
        }
    }
    function commit(id, note, scope) {
        if (settled)
            return
        if (id === "approve" && !scope)
            return choose(true)
        if (id === "deny")
            return choose(false)
        chosen = true
        decision(id, note, scope)
    }
    function sendReason() {
        if (reasonField.text.trim().length > 0)
            commit("denyWithReason", reasonField.text.trim(), "")
    }
    // Cancel: back to the decisions, nothing chosen.
    function back() {
        step = "decision"
        pendingAction = ""
        reasonField.text = ""
        card.forceActiveFocus()
    }
    // The asker's shortcuts, while the card (or a button on it) has the focus.
    // With double-press, the first press arms and the second commits.
    activeFocusOnTab: rich
    Keys.onPressed: event => {
        if (settled || !rich)
            return
        if (event.key === Qt.Key_Escape) {
            if (step !== "decision")
                back()
            else if (armed)
                armed = ""
            else
                return
            event.accepted = true
            return
        }
        if (step !== "decision" || (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)))
            return
        const typed = event.text.toLowerCase()
        const action = typed.length === 1 ? actions.find(a => a.key === typed) : undefined
        if (!action)
            return
        event.accepted = true
        // A held key repeats: only a fresh press arms or commits.
        if (event.isAutoRepeat)
            return
        if (doublePress && armed !== action.id) {
            armed = action.id
            return
        }
        armed = ""
        if (action.id === "approve")
            choose(true)
        else if (action.id === "deny")
            choose(false)
        else
            pick(action.id)
    }
    TapHandler { onTapped: if (card.rich && !card.settled) card.forceActiveFocus() }

    // A foot button that rises in by its order.
    component FootButton: AbstractButton {
        id: button
        property int order: 0
        height: 30
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        HoverHandler { cursorShape: card.settled ? Qt.ArrowCursor : Qt.PointingHandCursor }
        Entrance {
            id: riseIn
            duration: 450
            delay: 240 + button.order * 50
            curve: card.pop
        }
        Component.onCompleted: riseIn.play()
        // .approval.is-answered: .5, in 0.2 s.
        property real spent: card.settled ? 0.5 : 1
        Behavior on spent {
            enabled: !Theme.reducedMotion
            NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
        }
        opacity: Math.min(1, riseIn.value) * spent
        // :active scale .95 in 0.2 s on the motion curve, under the rise.
        property real press: down && !card.settled ? 0.95 : 1
        Behavior on press {
            enabled: !Theme.reducedMotion
            NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
        }
        scale: press * (0.92 + 0.08 * riseIn.value)
        transform: Translate { y: 8 * (1 - riseIn.value) }
    }
    // .approval-button: 30 px tall, 16 px either side, 13.5 px at 600.
    component Choice: FootButton {
        id: choice
        property color fill
        property color ink
        property string shortcut: ""
        property bool armedHere: false
        property string tip: ""
        enabled: !card.settled
        width: label.implicitWidth + 32
        Accessible.name: text
        Accessible.description: tip
        ToolTip.visible: hovered && (tip.length > 0 || shortcut.length > 0) && !card.settled
        ToolTip.delay: 600
        ToolTip.text: tip + (shortcut.length > 0 ? (tip.length > 0 ? "  ·  " : "") + "Key: " + shortcut : "")
        Accessible.role: Accessible.Button
        background: Rectangle {
            radius: 15
            antialiasing: true
            color: choice.fill
            Behavior on color { ColorAnimation { duration: 150 } }
            // box-shadow: 0 0 0 2px composer-bg, 0 0 0 4px focus-ring.
            FocusRing { target: choice; inset: 2 }
            // An armed shortcut: the same ring, shown until the second press.
            Rectangle {
                x: -4
                y: -4
                width: choice.width + 8
                height: choice.height + 8
                radius: height / 2
                visible: choice.armedHere
                color: "transparent"
                border.width: 2
                border.color: Theme.alpha(Theme.strong, 0.35)
            }
        }
        contentItem: Label {
            id: label
            text: choice.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: choice.ink
            font.pointSize: Theme.points(13.5)
            font.weight: Theme.weight(600)
        }
    }
    // A quieter decision: 12 px text, underlined while hovered.
    component Link: FootButton {
        id: link
        property string shortcut: ""
        property bool armedHere: false
        property string tip: ""
        enabled: !card.settled
        height: 22
        width: linkLabel.implicitWidth + 12
        Accessible.name: text
        Accessible.role: Accessible.Button
        Accessible.description: tip
        ToolTip.visible: hovered && !card.settled
        ToolTip.delay: 600
        ToolTip.text: tip + (shortcut.length > 0 ? (tip.length > 0 ? "  ·  " : "") + "Key: " + shortcut : "")
        background: Rectangle {
            radius: 11
            color: link.armedHere ? Theme.alpha(Theme.strong, 0.08) : "transparent"
            FocusRing { target: link; inset: 0 }
        }
        contentItem: Label {
            id: linkLabel
            x: 6
            text: link.text
            verticalAlignment: Text.AlignVCenter
            color: link.hovered && !card.settled ? Theme.text : Theme.secondary
            font.pixelSize: 12
            font.underline: link.hovered && !card.settled
        }
    }
    // The focus ring for a keyboard focus (:focus-visible): 2 px of the
    // focus colour, `inset` px out from the button over a composer gap.
    component FocusRing: Rectangle {
        required property AbstractButton target
        property real inset: 0
        x: -inset - 2
        y: -inset - 2
        width: target.width + 2 * (inset + 2)
        height: target.height + 2 * (inset + 2)
        radius: height / 2
        visible: target.visualFocus
        color: "transparent"
        border.width: 2
        border.color: Theme.alpha(Theme.strong, 0.35)
    }
    // .approval-code: the well (radius 10, well background, a fg .05 ring)
    // scrolling past 184 px, 12.5/19 mono at .88, 9 px down and 12 px in.
    component CodeWell: Flickable {
        id: well
        property string text
        readonly property real natural: codeText.height + 18
        width: parent ? parent.width : 0
        height: Math.min(184, natural)
        contentWidth: width
        contentHeight: natural
        clip: natural > 184
        interactive: natural > 184
        boundsBehavior: Flickable.StopAtBounds
        Rectangle {
            parent: well
            z: -1
            anchors.fill: parent
            radius: 10
            antialiasing: true
            color: Theme.wellBg
            border.width: 1
            border.color: Theme.alpha(Theme.strong, 0.05)
        }
        LiveText {
            id: codeText
            objectName: "approvalCode"
            x: 12
            y: 8 // 9 px down; 12.5 px mono sits 1 px higher in Chromium.
            width: well.width - 24
            padding: 0
            content: well.text
            frontend: card.frontend
            wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
            color: Theme.alpha(Theme.strong, 0.88)
            font.family: Theme.mono
            font.pointSize: Theme.points(12.5)
            font.weight: Font.Medium
            lineHeight: 19
        }
    }
    // .approval-diff: up to 14 lines removed and added (approval-card.js
    // lines()), 6 px padding, scrolling past 232 px.
    component DiffWell: Flickable {
        id: diffWell
        property string removed
        property string added
        readonly property var removedLines: removed.length > 0 ? card.lines(removed) : {shown: [], more: 0}
        readonly property var addedLines: added.length > 0 ? card.lines(added) : {shown: [], more: 0}
        readonly property real natural: rows.height + 12
        width: parent ? parent.width : 0
        height: Math.min(232, natural)
        contentWidth: width
        contentHeight: natural
        clip: natural > 232
        interactive: natural > 232
        boundsBehavior: Flickable.StopAtBounds
        Rectangle {
            parent: diffWell
            z: -1
            anchors.fill: parent
            radius: 10
            antialiasing: true
            color: Theme.wellBg
            border.width: 1
            border.color: Theme.alpha(Theme.strong, 0.05)
        }
        Column {
            id: rows
            objectName: "approvalDiff"
            y: 6
            width: diffWell.width
            Repeater {
                model: diffWell.removedLines.shown
                DiffLine {
                    required property string modelData
                    sign: "−"
                    text: modelData
                    tone: "removed"
                }
            }
            MoreLines { count: diffWell.removedLines.more }
            Repeater {
                model: diffWell.addedLines.shown
                DiffLine {
                    required property string modelData
                    sign: diffWell.removed.length > 0 ? "+" : ""
                    text: modelData
                    tone: diffWell.removed.length > 0 ? "added" : "new"
                }
            }
            MoreLines { count: diffWell.addedLines.more }
        }
    }
    // .approval-more: "N more lines", 12 px, 3 px down and 24 px in.
    component MoreLines: Label {
        property int count: 0
        visible: count > 0
        x: 24
        width: parent ? parent.width - 36 : 0
        height: 23
        topPadding: 3 + Theme.halfLeading(font, 20)
        text: count + " more lines"
        color: Theme.tertiary
        font.pixelSize: 12
        font.weight: Theme.weight(500)
    }
    // One .approval-line: a 16 px sign column (none for a new line) and its text.
    component DiffLine: Rectangle {
        id: diffLine
        objectName: "approvalDiffLine"
        property string sign
        property string text
        property string tone: "new" // new, added or removed
        width: parent ? parent.width : 0
        height: Math.max(1, lineText.lineCount) * 19
        color: tone === "added" ? Theme.alpha(Theme.success, 0.09)
             : tone === "removed" ? Theme.alpha(Theme.danger, 0.09) : "transparent"
        Label {
            x: 8
            visible: diffLine.tone !== "new"
            height: 19
            lineHeightMode: Text.FixedHeight
            lineHeight: 19
            topPadding: Theme.halfLeading(font, 19) - 1
            text: diffLine.sign
            color: diffLine.tone === "added" ? Theme.success
                 : diffLine.tone === "removed" ? Theme.danger : Theme.tertiary
            font: lineText.font
        }
        LiveText {
            id: lineText
            x: diffLine.tone === "new" ? 12 : 24
            y: -1
            width: diffLine.width - x - 12
            padding: 0
            lineHeight: 19
            content: diffLine.text || " "
            frontend: card.frontend
            wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
            color: diffLine.tone === "added" ? Theme.diffAdded
                 : diffLine.tone === "removed" ? Theme.diffRemoved : Theme.text
            font.family: Theme.mono
            font.pointSize: Theme.points(12.5)
            font.weight: Font.Medium
        }
    }
}
