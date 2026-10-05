import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import OpenGhost.Native

// The model stage (model-stage.js, .model-stage): the whole window steps
// back behind a blurred, darkened veil, and the models stand over it,
// right-aligned above the model button with the glyph column right over it,
// grouped under their providers when there is more than one. The chosen
// model carries the sparkle, which flies out of the button on the way in and
// back on the way out. The focused row is sharp and glows; the others are
// faint, soft and a little smaller, and bend away like a drum. Letters rise
// in row by row from the button, ripple when a row is reached, and dissolve
// upward when the stage closes. A pick hops the sparkle to its row first.
//
// The rows are Settings.choices: every available model, the pick going
// through Settings.choose with its thinking rule. Keys: Up/Down and
// Tab/Shift+Tab move round the rows, Home/End jump, Enter or Space picks,
// Escape closes. With reduced motion everything is at its end at once.
Popup {
    id: stage
    objectName: "modelStage"
    required property var settings
    required property Item button
    // The window's content, which the veil blurs, and the composer, whose
    // top the list grows up from.
    required property Item backdrop
    required property Item composer
    // Where focus goes back after a pointer close.
    property Item input

    parent: Overlay.overlay
    x: 0
    y: 0
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    padding: 0
    modal: false
    dim: false
    focus: true
    closePolicy: Popup.NoAutoClose
    background: null
    enter: Transition {}
    exit: Transition {}

    // closed, open, hop (a pick on its way), closing.
    property string phase: "closed"
    property bool keyboard: false
    property var rows: []
    property int focused: -1
    property int marked: -1
    property bool markAway: false
    readonly property int current: {
        if (stage.settings.invalid)
            return -1
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].provider === stage.settings.provider && rows[i].id === stage.settings.model)
                return i
        return -1
    }
    readonly property bool grouped: groups.length > 1
    property var groups: []

    // One clock for every timed part, in ms since the stage opened; it runs
    // only while something moves.
    property real now: 0
    property real busyUntil: 0
    property real closeAt: -1
    // A glyph in flight: from/to {x, y, size} in stage coordinates.
    property var flight: null

    // OPEN, CLOSE, HOP, WAVE, TWIRL and the easings of model-stage.js.
    readonly property real rowPace: Math.min(90, 480 / Math.max(1, rows.length))
    readonly property var curves: ({ motion: [0.32, 0.72, 0, 1], out: [0.22, 1, 0.36, 1],
                                     flight: [0.5, 0, 0.18, 1], spring: [0.34, 1.56, 0.64, 1],
                                     wave: [0.33, 1, 0.68, 1], inn: [0.42, 0, 1, 1] })
    function ease(curve, t) {
        const c = curves[curve]
        return t <= 0 ? 0 : t >= 1 ? 1 : Theme.bezier(c[0], c[1], c[2], c[3], t)
    }
    function phaseAt(start, duration) { return Math.max(0, Math.min(1, (now - start) / duration)) }
    function keep(until) {
        busyUntil = Math.max(busyUntil, until)
        if (!Theme.reducedMotion && !clock.running)
            clock.start()
    }
    // Long names keep the same rhythm: their letters follow closer.
    function stagger(count, step, spread) { return Math.min(step, spread / Math.max(1, count)) }
    // The veil: 0 … 1, in on the motion curve over 460 ms, out over 380 ms after 60.
    readonly property real veil: Theme.reducedMotion ? (phase === "closed" ? 0 : 1)
        : closeAt >= 0 ? 1 - ease("motion", phaseAt(closeAt + 60, 380))
        : ease("motion", phaseAt(0, 460))

    function place() {
        const b = button.mapToItem(null, 0, 0, button.width, button.height)
        const top = composer.mapToItem(null, 0, 0).y
        list.rightInset = Math.max(0, width - (b.x + b.width / 2) - 15 - 68)
        list.bottomAt = top - 26 + 10
        list.maxHeight = Math.max(180, top - 26 - 52 + 10)
    }
    onWidthChanged: if (phase !== "closed") place()
    onHeightChanged: if (phase !== "closed") place()

    function openStage(fromKeyboard) {
        if (phase !== "closed" || !settings.choices.length)
            return
        keyboard = fromKeyboard
        now = 0
        busyUntil = 0
        closeAt = -1
        flight = null
        markAway = false
        marked = -1
        const choices = settings.choices, seen = []
        for (const row of choices)
            if (!seen.includes(row.provider))
                seen.push(row.provider)
        groups = seen
        rows = choices
        phase = "open"
        place()
        open()
        button.expanded = true
        Theme.settle(column)
        middle(current)
        rest(current)
        if (current < 0)
            return
        if (Theme.reducedMotion) {
            marked = current
            return
        }
        keep(90 + (rows.length - 1) * rowPace + 560 + 420)
        launch(current)
    }
    function slotOf(index) { return repeater.itemAt(index) }
    function glyphCentre(item, size) {
        const p = item.mapToItem(stage.contentItem, item.width / 2, item.height / 2)
        return { x: p.x, y: p.y, size: size }
    }
    function buttonGlyph() { return glyphCentre(button.glyph, 16) }
    function markOf(index) {
        const slot = slotOf(index)
        return slot ? glyphCentre(slot.mark, 26) : null
    }
    // The button's glyph lifts off and lands next to the chosen model.
    function launch(index) {
        marked = index
        markAway = true
        button.away = true
        fly(buttonGlyph(), markOf(index), 40, 640, "flight", -70, -0.32, button.turn, () => {
            markAway = false
            slotOf(index)?.pop()
        })
    }
    function fly(from, to, delay, duration, curve, spin, bend, turn, done) {
        flight = { from: from, to: to, start: now + delay, duration: duration, curve: curve,
                   spin: spin, bend: bend, turn: turn, rest: Math.round(turn / 360) * 360, done: done }
        keep(now + delay + duration)
    }
    // Scrolls so the row sits in the middle of the list, where the drum is flat.
    function middle(index) {
        const slot = slotOf(index)
        if (!slot)
            return
        list.contentY = Math.max(0, Math.min(list.contentHeight - list.height,
                                             slot.y + slot.height / 2 - list.height / 2))
    }
    function rest(index) {
        if (index >= 0)
            reach(index, false)
    }
    function reach(index, wave) {
        if (index < 0 || index === focused)
            return
        focused = index
        const slot = slotOf(index)
        if (!slot)
            return
        slot.forceActiveFocus()
        if (wave && !Theme.reducedMotion)
            slot.ripple()
    }
    function pick(index) {
        if (phase !== "open" || index < 0)
            return
        if (index === current) {
            closeStage()
            return
        }
        phase = "hop"
        settings.choose(rows[index].provider, rows[index].id)
        if (Theme.reducedMotion || marked < 0 || markAway && !flight) {
            button.away = true
            marked = index
            markAway = false
            phase = "open"
            closeStage()
            return
        }
        const from = flight ? flyerCentre() : markOf(marked)
        const turn = flight ? flyer.turn : slotOf(marked).mark.stands
        marked = index
        markAway = true
        fly(from, markOf(index), 0, 420, "motion", -90, 0.5, turn, () => {
            markAway = false
            phase = "open"
            closeStage()
        })
    }
    function flyerCentre() {
        return { x: flyer.x + flyer.width / 2, y: flyer.y + flyer.height / 2, size: flyer.width * flyer.grow }
    }
    function cancel() {
        if (phase === "open")
            closeStage()
    }
    // Everything dissolves and the glyph flies home from wherever it stands.
    function closeStage() {
        if (phase !== "open")
            return
        phase = "closing"
        button.expanded = false
        if (Theme.reducedMotion) {
            finish()
            return
        }
        closeAt = now
        const home = buttonGlyph()
        if (flight || marked >= 0 && slotOf(marked)) {
            const from = flight ? flyerCentre() : markOf(marked)
            const turn = flight ? flyer.turn : slotOf(marked).mark.stands
            markAway = true
            fly(from, home, 0, 540, "motion", 70, -0.32, turn, () => land())
        } else {
            land()
        }
        keep(now + 60 + 380)
    }
    function land() {
        flight = null
        button.away = false
        button.land()
    }
    function finish() {
        clock.stop()
        flight = null
        button.away = false
        button.expanded = false
        phase = "closed"
        focused = -1
        marked = -1
        closeAt = -1
        close()
        if (keyboard)
            button.forceActiveFocus()
        else
            input?.forceActiveFocus()
    }
    function step() {
        if (flight && now >= flight.start + flight.duration) {
            const done = flight.done
            flight = null
            done?.()
        }
        if (phase === "closing" && now >= busyUntil && !flight)
            finish()
        else if (now >= busyUntil && !flight)
            clock.stop()
    }
    FrameAnimation {
        id: clock
        onTriggered: {
            stage.now += Math.min(frameTime, 0.05) * 1000
            stage.step()
        }
    }
    Connections {
        target: Theme
        function onReducedMotionChanged() {
            if (!Theme.reducedMotion)
                return
            if (stage.phase === "closing" || stage.phase === "hop") {
                stage.phase = "closing"
                stage.finish()
            } else if (stage.phase === "open") {
                stage.flight = null
                stage.markAway = false
                stage.button.away = true
                if (stage.marked < 0)
                    stage.marked = stage.current
                clock.stop()
            }
        }
    }
    onClosed: if (phase !== "closed") { phase = "closing"; finish() }

    contentItem: Item {
        focus: true
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape) {
                event.accepted = true
                stage.cancel()
                return
            }
            if (stage.phase !== "open" || !stage.rows.length)
                return
            const k = stage.focused, n = stage.rows.length
            const step = event.key === Qt.Key_Up || event.key === Qt.Key_Backtab ? -1
                : event.key === Qt.Key_Down || event.key === Qt.Key_Tab ? 1 : 0
            const to = step ? (k + step + n) % n
                : event.key === Qt.Key_Home ? 0 : event.key === Qt.Key_End ? n - 1 : -1
            if (to >= 0) {
                event.accepted = true
                stage.reach(to, true)
                list.reveal(stage.slotOf(to))
            } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        || event.key === Qt.Key_Space) && k >= 0) {
                event.accepted = true
                stage.pick(k)
            }
        }

        // The veil: blur(24px) saturate(1.1) brightness(.5) over
        // rgba(10, 10, 10, .36), as one blur and one rgba(5, 5, 5, .68).
        ShaderEffectSource {
            id: behind
            anchors.fill: parent
            sourceItem: stage.visible ? stage.backdrop : null
            live: true
            visible: false
        }
        MultiEffect {
            anchors.fill: parent
            source: behind
            autoPaddingEnabled: false
            blurEnabled: true
            // MultiEffect's blur is about σ = .27 · blurMax · blur · (1 + multiplier): 24 px.
            blurMax: 64
            blurMultiplier: 0.4
            blur: stage.veil
            saturation: 0.1 * stage.veil
            // Light: brightness(1.08), which clips everything from 236 up to
            // white; MultiEffect adds rather than scales (the same at the top).
            brightness: Theme.light ? 0.08 * stage.veil : 0
        }
        // brightness(.5) under rgba(10, 10, 10, .36): c·.32 + 3.6, as rgba(5, 5, 5, .68).
        // Light: the brightened picture under rgba(250, 250, 250, .5).
        Rectangle {
            anchors.fill: parent
            color: Theme.light ? Qt.rgba(250 / 255, 250 / 255, 250 / 255, 0.5 * stage.veil)
                               : Qt.rgba(5 / 255, 5 / 255, 5 / 255, 0.68 * stage.veil)
        }
        // A press on the veil, or between the rows, closes the stage.
        MouseArea {
            anchors.fill: parent
            onPressed: stage.cancel()
        }

        // .model-list: padding 12 68 10 96; rows end at its right padding.
        Flickable {
            id: list
            property real rightInset: 0
            property real bottomAt: 0
            property real maxHeight: 180
            readonly property bool canUp: contentY > 2
            readonly property bool canDown: contentY + height < contentHeight - 2
            // On whole pixels: the fade's strips clip on them.
            x: Math.round(stage.width - rightInset - width)
            y: Math.round(bottomAt - height)
            width: Math.ceil(column.width + 68 + 96)
            height: Math.round(Math.min(contentHeight, maxHeight))
            contentWidth: width
            contentHeight: column.height + 12 + 10
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            // Whatever runs past the top or bottom fades into the veil (72 px).
            layer.enabled: canUp || canDown
            layer.effect: Feather {
                opaque: false
                topEdge: list.canUp ? 72 : 0
                bottomEdge: list.canDown ? 72 : 0
            }
            function reveal(slot) {
                if (!slot)
                    return
                const top = slot.y + 12, bottom = top + slot.height
                if (top < contentY)
                    contentY = top
                else if (bottom > contentY + height)
                    contentY = bottom - height
            }
            onContentYChanged: stage.follow()
            HoverHandler {
                onHoveredChanged: if (!hovered && stage.phase === "open") stage.rest(stage.current)
            }
            MouseArea { // Between the rows: the same as the veil.
                width: list.contentWidth
                height: list.contentHeight
                onPressed: stage.cancel()
            }
            // Rows (each with its group label when it starts a group) end at
            // the column's right edge.
            Column {
                id: column
                x: 96
                y: 12
                Repeater {
                    id: repeater
                    model: stage.rows
                    delegate: StageRow {}
                }
            }
        }
        // The sparkle on its way between the button and a row.
        ModelGlyph {
            id: flyer
            readonly property var f: stage.flight
            readonly property real t: f ? stage.ease(f.curve, stage.phaseAt(f.start, f.duration)) : 0
            readonly property real u: 1 - t
            readonly property var c: f ? { "x": (f.from.x + f.to.x) / 2 - (f.to.y - f.from.y) * f.bend,
                                              "y": (f.from.y + f.to.y) / 2 + (f.to.x - f.from.x) * f.bend } : null
            readonly property real grow: f ? (f.from.size + (f.to.size - f.from.size) * t) / 26 : 1
            visible: f !== null
            width: 26
            height: 26
            x: f ? u * u * f.from.x + 2 * u * t * c.x + t * t * f.to.x - 13 : 0
            y: f ? u * u * f.from.y + 2 * u * t * c.y + t * t * f.to.y - 13 : 0
            turn: f ? f.turn + (f.rest - f.turn) * t : 0
            color: Theme.accent
            transform: [
                Scale { origin.x: 13; origin.y: 13; xScale: flyer.grow; yScale: flyer.grow },
                Rotation { origin.x: 13; origin.y: 13; angle: flyer.f ? flyer.f.spin * flyer.u : 0 }
            ]
        }
    }

    // Keeps focus with the row under the pointer, or the one in the middle,
    // while the list scrolls.
    property point pointer: Qt.point(-1, -1)
    function follow() {
        if (phase !== "open")
            return
        let best = -1, distance = Infinity
        const mid = list.contentY + list.height / 2
        for (let i = 0; i < repeater.count; ++i) {
            const slot = repeater.itemAt(i)
            if (!slot)
                continue
            const at = slot.mapToItem(stage.contentItem, 0, 0)
            if (pointer.x >= at.x && pointer.x < at.x + slot.width && pointer.y >= at.y
                && pointer.y < at.y + slot.height) {
                best = i
                break
            }
            const d = Math.abs(slot.y + 12 + slot.height / 2 - mid)
            if (d < distance) {
                distance = d
                best = i
            }
        }
        if (best >= 0 && best !== focused && list.moving)
            reach(best, false)
    }

    component StageRow: Item {
        id: slot
        required property int index
        required property var modelData
        readonly property bool isFocus: stage.focused === index
        readonly property bool heads: stage.grouped
            && (index === 0 || stage.rows[index - 1].provider !== modelData.provider)
        readonly property Item mark: mark
        readonly property real nameSize: Math.max(28, Math.min(44, stage.width * 0.033))
        readonly property real letterStep: stage.stagger(letters.count, 26, 420)
        // Rows rise from the button upwards: the nearest one first.
        readonly property real base: 90 + (stage.rows.length - 1 - index) * stage.rowPace
        readonly property real waveStep: stage.stagger(letters.count, 22, 300)
        property real waveAt: -1e9
        property real popAt: -1e9
        // The drum: rows away from the list's middle drift right, tilt and shrink.
        readonly property real drum: {
            if (Theme.reducedMotion)
                return 0
            const centre = y + 12 + height / 2
            return Math.max(-1.3, Math.min(1.3, (centre - list.contentY - list.height / 2) / 320))
        }
        x: column.width - width
        width: implicitWidth
        implicitWidth: Math.max(row.implicitWidth, group.implicitWidth + 48)
        height: (heads ? group.height : 0) + row.height
        activeFocusOnTab: false
        Accessible.role: Accessible.ListItem
        Accessible.name: modelData.name + ", " + (modelData.imageInput ? "Sees photos" : "No photos")
        Accessible.selected: index === stage.current

        function ripple() {
            waveAt = stage.now
            stage.keep(stage.now + 560 + letters.count * waveStep)
            if (stage.marked === index && !stage.markAway) {
                // A twirl under way hands over from wherever it has got to;
                // the big star stops on a quarter, the small one keeps the turn.
                twirl.stop()
                twirlBig = mark.bigTurn
                twirlTurn = mark.turn
                twirl.start()
            }
        }
        property real twirlBig: 0
        property real twirlTurn: 0
        function pop() { popAt = stage.now; stage.keep(stage.now + 520) }

        // .model-group: 11 px/14 px 700, .14em apart, rgba(255, 255, 255, .4).
        Text {
            id: group
            visible: slot.heads
            anchors.right: parent.right
            anchors.rightMargin: 48
            topPadding: 18
            bottomPadding: 2
            height: 34
            text: slot.modelData.providerName.toUpperCase()
            color: Theme.alpha(Theme.strong, 0.4)
            font.pixelSize: 11
            font.weight: Theme.weight(700)
            font.letterSpacing: 11 * 0.14
            lineHeight: 14
            lineHeightMode: Text.FixedHeight
            readonly property real first: 90 + (stage.rows.length - 1 - slot.index) * stage.rowPace
            opacity: Theme.reducedMotion ? 1 : stage.closeAt >= 0
                ? 1 - stage.phaseAt(stage.closeAt, 200)
                : stage.ease("out", stage.phaseAt(first, 420))
            transform: [
                Scale { origin.x: group.width; origin.y: group.height / 2; xScale: 1 - 0.07 * Math.abs(slot.drum); yScale: xScale },
                Rotation { origin.x: group.width; origin.y: group.height / 2; angle: -3.2 * slot.drum },
                Translate { x: 34 * slot.drum * slot.drum }
            ]
        }

        // .model-row: 10 px above and below; the name and its line, 18 px, the mark.
        Item {
            id: row
            objectName: "modelRow"
            y: slot.heads ? group.height : 0
            anchors.right: parent.right
            implicitWidth: nameBlock.width + 18 + 30
            width: implicitWidth
            height: Math.max(nameBlock.height, 30) + 20
            // Focus: opacity .26 → 1 (.35 s ease), blur 1.5 px → none
            // (.45 s), scale .95 → 1 (.6 s), glow in .5 s.
            property real lit: slot.isFocus ? 1 : 0
            property real sharp: slot.isFocus ? 1 : 0
            property real grow: slot.isFocus ? 1 : 0
            property real glow: slot.isFocus ? 1 : 0
            Behavior on lit { enabled: !Theme.reducedMotion; NumberAnimation { duration: 350; easing.type: Easing.Bezier; easing.bezierCurve: [0.25, 0.1, 0.25, 1, 1, 1] } }
            Behavior on sharp { enabled: !Theme.reducedMotion; NumberAnimation { duration: 450; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
            Behavior on grow { enabled: !Theme.reducedMotion; NumberAnimation { duration: 600; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
            Behavior on glow { enabled: !Theme.reducedMotion; NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: [0.25, 0.1, 0.25, 1, 1, 1] } }
            opacity: 0.26 + 0.74 * lit
            layer.enabled: sharp < 0.999
            layer.effect: MultiEffect {
                blurEnabled: true
                blurMax: 8
                blur: 0.7 * (1 - row.sharp)
            }
            transform: [
                Scale { origin.x: row.width; origin.y: row.height / 2; xScale: 0.95 + 0.05 * row.grow; yScale: xScale },
                Scale { origin.x: row.width; origin.y: row.height / 2; xScale: 1 - 0.07 * Math.abs(slot.drum); yScale: xScale },
                Rotation { origin.x: row.width; origin.y: row.height / 2; angle: -3.2 * slot.drum },
                Translate { x: 34 * slot.drum * slot.drum }
            ]

            // The name over its line, right-aligned, 8 px apart.
            Item {
                id: nameBlock
                y: (row.height - height) / 2
                width: Math.max(name.width, meta.width)
                height: name.height + 8 + meta.height
                Item {
                    id: name
                    anchors.right: parent.right
                    // Each letter is its own box, .025 em tighter, the last one too.
                    readonly property real spacing: -0.025 * slot.nameSize
                    width: letterRow.implicitWidth + spacing
                    height: slot.nameSize
                    // text-shadow: 0 0 34px rgba(var(--glow-rgb), .28) on focus; on
                    // the milky light veil 0 0 30px at .1.
                    layer.enabled: row.glow > 0.001
                    layer.effect: MultiEffect {
                        shadowEnabled: true
                        // MultiEffect's shadow is not Chromium's text-shadow:
                        // this strength and spread match its measured falloff.
                        shadowColor: Theme.glow
                        shadowOpacity: 1.5 * (Theme.light ? 0.1 : 0.28) * row.glow
                        shadowBlur: 1
                        blurMax: 40
                        shadowHorizontalOffset: 0
                        shadowVerticalOffset: 0
                    }
                    Row {
                        id: letterRow
                        spacing: name.spacing
                        Repeater {
                            id: letters
                            model: Array.from(slot.modelData.name)
                            delegate: Text {
                                id: letter
                                required property int index
                                required property string modelData
                                text: modelData
                                color: Theme.accent
                                font.pointSize: Theme.points(slot.nameSize)
                                font.weight: Theme.weight(650)
                                height: slot.nameSize
                                verticalAlignment: Text.AlignVCenter
                                // Open: rise .5 em, fade and sharpen from 8 px (560 ms,
                                // out). Close: last first, .3 em up into a 6 px blur
                                // (240 ms, ease-in). Wave: .14 em up at 38 % (560 ms).
                                readonly property real rise: Theme.reducedMotion ? 1
                                    : stage.ease("out", stage.phaseAt(slot.base + index * slot.letterStep, 560))
                                readonly property real leave: Theme.reducedMotion || stage.closeAt < 0 ? 0
                                    : stage.ease("inn", stage.phaseAt(stage.closeAt + (letters.count - 1 - index)
                                        * stage.stagger(letters.count, 12, 160), 240))
                                readonly property real wave: {
                                    const e = stage.ease("wave", stage.phaseAt(slot.waveAt + index * slot.waveStep, 560))
                                    return e <= 0 || e >= 1 ? 0 : e < 0.38 ? e / 0.38 : (1 - e) / 0.62
                                }
                                readonly property real soft: 8 * (1 - rise) + 6 * leave
                                opacity: rise * (1 - leave)
                                transform: Translate {
                                    y: slot.nameSize * (0.5 * (1 - letter.rise) - 0.3 * letter.leave - 0.14 * letter.wave)
                                }
                                layer.enabled: soft > 0.2
                                layer.effect: MultiEffect {
                                    blurEnabled: true
                                    blurMax: 32
                                    blur: Math.min(1, letter.soft / 8.6)
                                }
                            }
                        }
                    }
                }
                // .model-meta: 13 px 500, rgba(255, 255, 255, .55), .01 em apart.
                Text {
                    id: meta
                    anchors.right: parent.right
                    y: name.height + 8
                    height: 13
                    text: slot.modelData.imageInput ? "Sees photos" : "No photos"
                    color: Theme.alpha(Theme.strong, 0.55)
                    font.pixelSize: 13
                    font.weight: Theme.weight(500)
                    font.letterSpacing: 0.13
                    verticalAlignment: Text.AlignVCenter
                    readonly property real rise: Theme.reducedMotion ? 1
                        : stage.ease("out", stage.phaseAt(slot.base + 180, 480))
                    opacity: rise * (stage.closeAt < 0 ? 1 : 1 - stage.phaseAt(stage.closeAt, 200))
                    transform: Translate { y: 6 * (1 - meta.rise) }
                }
            }
            // .model-mark: the living sparkle, 26 px in a 30 px box.
            Item {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 30
                height: 30
                ModelGlyph {
                    id: mark
                    anchors.centerIn: parent
                    width: 26
                    height: 26
                    color: Theme.accent
                    alive: true
                    visible: stage.marked === slot.index && !stage.markAway
                    readonly property real popped: stage.phaseAt(slot.popAt, 520)
                    scale: popped > 0 && popped < 1 ? 1.3 - 0.3 * stage.ease("spring", popped) : 1
                }
            }
            // The glyph greets its own model with a half turn and a swell.
            SequentialAnimation {
                id: twirl
                ParallelAnimation {
                    NumberAnimation { target: mark; property: "bigTurn"; to: (slot.twirlBig + Math.round((slot.twirlBig + 180) / 90) * 90) / 2; duration: 342; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                    NumberAnimation { target: mark; property: "bigScale"; to: 1.2; duration: 342; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                    NumberAnimation { target: mark; property: "turn"; to: slot.twirlTurn + 90; duration: 342; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                }
                ParallelAnimation {
                    NumberAnimation { target: mark; property: "bigTurn"; to: Math.round((slot.twirlBig + 180) / 90) * 90; duration: 418; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                    NumberAnimation { target: mark; property: "bigScale"; to: 1; duration: 418; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                    NumberAnimation { target: mark; property: "turn"; to: slot.twirlTurn + 180; duration: 418; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                }
            }
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onEntered: if (stage.phase === "open") stage.reach(slot.index, true)
                onPositionChanged: mouse => stage.pointer = mapToItem(stage.contentItem, mouse.x, mouse.y)
                onClicked: stage.pick(slot.index)
            }
        }
    }
}
