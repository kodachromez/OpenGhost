import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Shapes
import OpenGhost.Native

// The agent mode's dock (OpenGhost 1.3 dock.js as mode-picker.js uses it,
// a choosing Dock): the modes as icons in a capsule grown out of the mode
// button, a glass lens over the one in focus with a dot under the current
// one, and above the capsule that mode's name in large type with its line
// under it, over a soft patch of the selection's veil. The icons fly out of
// the button nearest first and land on a spring; the name's letters fly out
// of the lens; when the lens moves on the old name melts upward. A pick
// moves the dot, waits a moment, and the dock folds back into the button
// while the chosen icon flies home into it.
//
// Placed over the window (Overlay). Keys: Left/Up and Right/Down move round
// the modes, Home/End jump, Enter or Space picks, Escape or Tab closes.
// A press outside closes it; so does a resize. Reduced motion: every part
// at its end at once.
Item {
    id: dock
    objectName: "modeDock"
    required property Item button // ModePicker
    required property Item backdrop // The window content the patch frosts.
    // A mode was picked here: asks Ghosty for it (setPermissionMode).
    signal picked(string id)

    anchors.fill: parent
    visible: phase !== "closed"
    z: 100

    // closed, open, closing.
    property string phase: "closed"
    readonly property bool open: phase === "open"
    property bool keyboard: false
    property bool focusButton: false
    property bool choosing: false
    property var items: [] // The button's choices when it opened.
    property string current: "" // The dot's mode.
    property int focused: -1
    property int named: -1
    property real focusedAt: 0

    // dock.js geometry: an icon's slot, the capsule's padding, the lens.
    readonly property real slotWidth: 46
    readonly property real slotHeight: 38
    readonly property real pad: 5
    readonly property var lensSize: ({width: 42, height: 34})
    // Where the button and its icons were when the dock opened.
    property rect home: Qt.rect(0, 0, 0, 0)
    property rect origin: Qt.rect(0, 0, 0, 0)
    readonly property point from: Qt.point(origin.x + origin.width / 2, origin.y + origin.height / 2)
    readonly property real panelWidth: 2 * pad + items.length * slotWidth
    readonly property real panelHeight: 2 * pad + slotHeight
    // The first icon stands over the button's glyph; the capsule 10 px above it.
    readonly property real panelX: from.x - (pad + slotWidth / 2)
    readonly property real panelY: home.y - 10 - panelHeight
    // .dock-word-box: clamp(28px, 2.9vw, 38px).
    readonly property real size: Math.max(28, Math.min(38, width * 0.029))

    function slotX(i) { return pad + slotWidth * (i + 0.5) }
    // The lens's place between the slots (dock.js x()).
    function xAt(pos) {
        const last = items.length - 1
        if (last < 0)
            return 0
        if (pos <= 0)
            return slotX(0) + pos * slotWidth
        if (pos >= last)
            return slotX(last) + (pos - last) * slotWidth
        return slotX(Math.floor(pos)) + slotWidth * (pos - Math.floor(pos))
    }
    function indexOf(id) {
        for (let i = 0; i < items.length; ++i)
            if (items[i].id === id)
                return i
        return -1
    }

    // dock.js spring(): a real spring sampled until it settles (response,
    // bounce); `at` its progress at t ms, `duration` how long it lasts.
    function spring(response, bounce) {
        const zeta = 1 - bounce, w = 2 * Math.PI / response, d = w * Math.sqrt(Math.max(0, 1 - zeta * zeta))
        const envelope = t => d ? Math.exp(-zeta * w * t) * Math.hypot(1, zeta * w / d) : Math.exp(-w * t) * (1 + w * t)
        let settle = 0.05
        while (envelope(settle) > 0.002)
            settle += 0.01
        const steps = 64, points = []
        for (let i = 0; i <= steps; ++i) {
            const t = settle * i / steps
            points.push(i === steps ? 1 : +(d ? 1 - Math.exp(-zeta * w * t) * (Math.cos(d * t) + zeta * w / d * Math.sin(d * t))
                                              : 1 - Math.exp(-w * t) * (1 + w * t)).toFixed(4))
        }
        return {points: points, duration: Math.round(settle * 1000)}
    }
    // CSS linear() through the sampled points.
    function sampled(s, p) {
        if (p <= 0)
            return 0
        if (p >= 1)
            return 1
        const x = p * (s.points.length - 1), i = Math.floor(x)
        return s.points[i] + (s.points[i + 1] - s.points[i]) * (x - i)
    }
    readonly property var grow: spring(0.62, 0.12)
    readonly property var land: spring(0.6, 0.1)
    readonly property var curves: ({motion: [0.32, 0.72, 0, 1], out: [0.22, 1, 0.36, 1], leave: [0.4, 0, 1, 1],
                                     flight: [0.3, 0.6, 0.2, 1], ease: [0.25, 0.1, 0.25, 1], inn: [0.42, 0, 1, 1]})
    function ease(curve, t) {
        const c = curves[curve]
        return t <= 0 ? 0 : t >= 1 ? 1 : Theme.bezier(c[0], c[1], c[2], c[3], t)
    }
    function progress(start, duration) { return duration <= 0 ? 1 : Math.max(0, Math.min(1, (now - start) / duration)) }
    function backOut(v) { return 1 + 2.2 * Math.pow(v - 1, 3) + 1.2 * Math.pow(v - 1, 2) }

    // One clock in ms (Theme.clock()), read on every frame while anything moves.
    property real now: 0
    property real busyUntil: 0
    function keep(until) {
        busyUntil = Math.max(busyUntil, until)
        if (!Theme.reducedMotion && !clock.running)
            clock.start()
    }
    // The lens between icons: a spring with a little overshoot (GLIDE),
    // stretched along the way by its speed (STRETCH) and pressed when it
    // names something or something is chosen (PRESS).
    property real pos: 0
    property real vel: 0
    property real goal: 0
    property real stretch: 0
    property real stretchVel: 0
    property real press: 0
    property real pressVel: 0
    property real revealAt: -1
    property real reveal: 1
    readonly property bool lensMoving: reveal < 1 || Math.abs(goal - pos) > 0.0005 || Math.abs(vel) > 0.002
                                       || Math.abs(stretch) > 0.001 || Math.abs(stretchVel) > 0.005
                                       || Math.abs(press) > 0.001 || Math.abs(pressVel) > 0.005
    FrameAnimation {
        id: clock
        onTriggered: dock.tick(frameTime)
    }
    function tick(frameTime) {
        now = Theme.clock()
        if (Theme.reducedMotion) {
            pos = goal
            vel = stretch = stretchVel = press = pressVel = 0
            reveal = 1
            clock.stop()
            return
        }
        const dt = Math.max(0, Math.min(frameTime, 0.032)), steps = Math.max(1, Math.ceil(dt / 0.004)), h = dt / steps
        for (let n = 0; n < steps; ++n) {
            vel += ((goal - pos) * 260 - vel * 28) * h
            pos += vel * h
            stretchVel += ((Math.min(0.28, Math.abs(vel) * 0.03) - stretch) * 520 - stretchVel * 34) * h
            stretch += stretchVel * h
            pressVel += ((0 - press) * 420 - pressVel * 30) * h
            press += pressVel * h
        }
        if (revealAt < 0)
            revealAt = now + 360 // OPEN.lensDelay, from the lens's first frame.
        reveal = Math.max(0, Math.min(1, (now - revealAt) / 320))
        if (!lensMoving) {
            pos = goal
            vel = stretch = stretchVel = press = pressVel = 0
        }
        aim()
        sweep()
        if (!lensMoving && now >= busyUntil && phase !== "closing")
            clock.stop()
        if (phase === "closing" && now >= closingAt + 610)
            finish()
    }
    function kick(amount) {
        if (Theme.reducedMotion)
            return
        pressVel += amount
        keep(now)
    }

    // The name follows the lens once it is nearly at its icon (.3 of a
    // slot) or the focus has stayed 90 ms, so a sweep names nothing.
    function aim() {
        if (phase !== "open" || named === focused || focused < 0)
            return
        if (Math.abs(pos - focused) > 0.3 && Theme.clock() - focusedAt < 90)
            return
        name(focused, 0)
        kick(8)
    }
    function focusAt(index) {
        if (index < 0 || index >= items.length)
            return
        if (index !== focused) {
            focused = index
            goal = index
            focusedAt = Theme.clock()
            keep(now)
        }
    }

    // Names on stage, the newest last; older ones melting away, and the
    // lines under them.
    ListModel { id: words }
    property real hintsAt: 0 // When the shown line began to come in.
    property int hinted: -1
    property real hintDelay: 140
    property real closingAt: -1
    property bool instantPatch: true
    function name(index, delay) {
        const item = items[index]
        if (!item)
            return
        now = Theme.clock()
        named = index
        for (let i = 0; i < words.count; ++i)
            if (words.get(i).leftAt < 0)
                words.setProperty(i, "leftAt", now)
        const lens = lensCentre()
        words.append({text: item.name, born: now, delay: delay, leftAt: -1, fx: lens.x, fy: lens.y})
        hinted = item.hint ? index : -1
        hintsAt = now
        keep(now + delay + 680 + 26 * item.name.length + 600)
        Qt.callLater(() => { dock.instantPatch = false })
    }
    function sweep() {
        for (let i = words.count - 1; i >= 0; --i) {
            const left = words.get(i).leftAt
            if (left >= 0 && now > left + 230 + 10 * 40)
                words.remove(i)
        }
    }
    function lensCentre() {
        return Qt.point(panelX + Math.round(xAt(pos)), panelY + pad + slotHeight / 2)
    }

    // Each icon's flight out of the button: its delay, nearest first.
    property var flights: []
    // When the dot comes in (once its icon is nearly there).
    property real dotAt: 0

    function toggle(byKey) {
        if (phase === "open")
            close(byKey)
        else
            show(byKey)
    }
    function show(byKey) {
        if (phase === "open")
            return
        if (phase === "closing")
            finish()
        const choices = button.choices
        if (!choices.length)
            return
        now = Theme.clock()
        items = choices
        current = button.mode
        keyboard = byKey
        focusButton = false
        choosing = false
        const h = button.mapToItem(dock, 0, 0, button.width, button.height)
        home = Qt.rect(h.x, h.y, h.width, h.height)
        const o = button.icons.mapToItem(dock, 0, 0, button.icons.width, button.icons.height)
        origin = Qt.rect(o.x, o.y, o.width, o.height)
        phase = "open"
        openedAt = now
        closingAt = -1
        words.clear()
        const start = Math.max(0, indexOf(current))
        focused = -1
        named = -1
        focusAt(start)
        pos = goal
        vel = stretch = stretchVel = press = pressVel = 0
        reveal = Theme.reducedMotion ? 1 : 0
        revealAt = -1
        // The icons fly nearest first: 30 ms, then 50 ms each.
        const order = items.map((item, i) => ({i: i, d: Math.hypot(from.x - (panelX + slotX(i)),
                                                                    from.y - (panelY + pad + slotHeight / 2))}))
                           .sort((a, b) => a.d - b.d)
        const list = []
        order.forEach((spot, k) => { list[spot.i] = 30 + k * 50 })
        flights = list
        dotAt = (flights[start] ?? 0) + land.duration * 0.5
        button.away = true
        named = start
        instantPatch = true
        hintDelay = 560
        name(start, 360)
        hintDelay = 140
        dock.forceActiveFocus()
        keep(now + 1400)
    }
    property real openedAt: 0
    // Choosing: the lens gives under the press, the dot moves there, and
    // after 560 ms the dock goes.
    function pick(index, byKey) {
        const item = items[index]
        if (!item || phase !== "open" || choosing)
            return
        focusAt(index)
        kick(14)
        choosing = true
        current = item.id
        dock.picked(item.id)
        chosenTimer.byKey = byKey
        chosenTimer.interval = Theme.reducedMotion ? 0 : 560
        chosenTimer.restart()
    }
    Timer {
        id: chosenTimer
        property bool byKey: false
        onTriggered: {
            dock.choosing = false
            dock.close(byKey)
        }
    }
    property bool returned: true
    property bool homing: false // The current icon flies home (a choosing dock's close).
    function close(byKey) {
        if (phase !== "open")
            return
        focusButton = byKey
        now = Theme.clock()
        if (Theme.reducedMotion) {
            finish()
            return
        }
        phase = "closing"
        closingAt = now
        returned = false
        homing = indexOf(current) >= 0
        backTimer.interval = homing ? 560 * 0.5 : 80 + 480 * 0.62
        backTimer.restart()
        keep(now + 620)
    }
    Timer {
        id: backTimer
        onTriggered: dock.back()
    }
    function back() {
        backTimer.stop()
        if (returned && !button.away)
            return
        returned = true
        button.home()
    }
    function finish() {
        phase = "closed"
        back()
        clock.stop()
        choosing = false
        words.clear()
        named = -1
        hinted = -1
        if (focusButton)
            button.forceActiveFocus(Qt.TabFocusReason)
    }
    // A window changing size moves the button out from under the stage.
    onWidthChanged: if (open) close(false)
    onHeightChanged: if (open) close(false)

    focus: open
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            event.accepted = true
            close(true)
            return
        }
        if (!open || choosing)
            return
        const n = items.length
        let to = -1
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Up)
            to = (focused - 1 + n) % n
        else if (event.key === Qt.Key_Right || event.key === Qt.Key_Down)
            to = (focused + 1) % n
        else if (event.key === Qt.Key_Home)
            to = 0
        else if (event.key === Qt.Key_End)
            to = n - 1
        else if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            event.accepted = true
            if (!event.isAutoRepeat)
                pick(focused, true)
            return
        }
        if (to < 0)
            return
        event.accepted = true
        keyboard = true
        focusAt(to)
    }

    // A press anywhere else puts it away; the button itself toggles it.
    MouseArea {
        anchors.fill: parent
        enabled: dock.open
        onPressed: mouse => {
            const p = dock.mapToItem(dock.button, mouse.x, mouse.y)
            if (!dock.button.contains(p))
                dock.close(false)
            else
                mouse.accepted = false
        }
    }

    // The patch behind the name, its line and the capsule (dock.js
    // StageVeil): the selection's veil, its edges fading over 64 px, 18 px
    // past them at full strength. It blooms out of the button from .45
    // (560 ms) and lifts 170 ms into the close (440 ms); it follows a new
    // name in .5 s.
    readonly property real veil: {
        if (Theme.reducedMotion)
            return phase === "closed" ? 0 : 1
        if (phase === "closing")
            return 1 - ease("motion", progress(closingAt + 170, 440))
        return ease("motion", progress(openedAt, 560))
    }
    readonly property rect stageBox: {
        let left = 0, top = 0, right = panelWidth, bottom = panelHeight
        const word = wordBox.newest
        if (word) {
            left = Math.min(left, wordBox.x)
            right = Math.max(right, wordBox.x + word.width)
            top = Math.min(top, wordBox.y)
        }
        if (hinted >= 0 && hintBox.shownWidth > 0) {
            right = Math.max(right, hintBox.x + hintBox.shownWidth)
            top = Math.min(top, hintBox.y)
        }
        const edge = 18 + 64
        return Qt.rect(panelX + left - edge, panelY + top - edge, right - left + 2 * edge, bottom - top + 2 * edge)
    }
    Item {
        id: patch
        objectName: "modeDockVeil"
        visible: dock.veil > 0.001
        x: dock.stageBox.x
        y: dock.stageBox.y
        width: dock.stageBox.width
        height: dock.stageBox.height
        Behavior on x { enabled: !dock.instantPatch && !Theme.reducedMotion; NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
        Behavior on y { enabled: !dock.instantPatch && !Theme.reducedMotion; NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
        Behavior on width { enabled: !dock.instantPatch && !Theme.reducedMotion; NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
        Behavior on height { enabled: !dock.instantPatch && !Theme.reducedMotion; NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
        readonly property real bloom: 0.45 + 0.55 * dock.veil
        transform: Scale {
            origin.x: dock.from.x - patch.x
            origin.y: dock.from.y - patch.y
            xScale: patch.bloom
            yScale: patch.bloom
        }
        layer.enabled: true
        layer.effect: Feather {
            leftEdge: 64
            topEdge: 64
            rightEdge: 64
            bottomEdge: 64
        }
        ShaderEffectSource {
            id: behind
            anchors.fill: parent
            visible: false
            live: true
            sourceItem: patch.visible ? dock.backdrop : null
            sourceRect: {
                void patch.bloom; void patch.x; void patch.y; void patch.width; void patch.height
                return patch.visible && dock.backdrop
                    ? patch.mapToItem(dock.backdrop, 0, 0, patch.width, patch.height) : Qt.rect(0, 0, 0, 0)
            }
        }
        // blur(5px), growing with the veil: a Gaussian across, then down.
        ShaderEffect {
            id: veilAcross
            anchors.fill: parent
            property var source: behind
            property vector2d delta: Qt.vector2d(1 / width, 0)
            property real sigma: 5 * dock.veil
            property real goo: 0
            property vector4d tint: Qt.vector4d(0, 0, 0, 0)
            fragmentShader: "qrc:/shaders/blur.frag.qsb"
        }
        ShaderEffectSource { id: veilAcrossTexture; sourceItem: veilAcross; hideSource: true; visible: false }
        ShaderEffect {
            anchors.fill: parent
            property var source: veilAcrossTexture
            property vector2d delta: Qt.vector2d(0, 1 / height)
            property real sigma: 5 * dock.veil
            property real goo: 0
            property vector4d tint: Qt.vector4d(0, 0, 0, 0)
            fragmentShader: "qrc:/shaders/blur.frag.qsb"
        }
        // brightness(.78) contrast(.949), or the light theme's contrast(.6) brightness(1.25).
        Rectangle {
            anchors.fill: parent
            color: Theme.light ? Qt.rgba(1, 1, 1, 0.25 * dock.veil)
                               : Qt.rgba(25 / 255, 25 / 255, 25 / 255, 0.26 * dock.veil)
        }
    }

    // .dock: the capsule and its icons, in the window's coordinates.
    Item {
        id: panel
        objectName: "modeDockPanel"
        x: dock.panelX
        y: dock.panelY
        width: dock.panelWidth
        height: dock.panelHeight

        // The capsule and the icons, which the lens magnifies.
        Item {
            id: under
            anchors.fill: parent
            // .dock-shell: grows out of the button (GROW spring), and folds
            // back into it 80 ms into the close (480 ms, the motion curve),
            // fading only past .6 of the way.
            Item {
                id: shell
                objectName: "modeDockShell"
                readonly property real g: {
                    if (dock.phase === "closing") {
                        const t = dock.progress(dock.closingAt + 80, 480)
                        return 1 - dock.ease("motion", t)
                    }
                    return Theme.reducedMotion ? 1 : dock.sampled(dock.grow, dock.progress(dock.openedAt, dock.grow.duration))
                }
                readonly property real fade: dock.phase === "closing"
                    ? 1 - Math.max(0, (dock.progress(dock.closingAt + 80, 480) - 0.6) / 0.4)
                    : Math.min(1, g / 0.1)
                readonly property real bx: dock.home.x - dock.panelX
                readonly property real by: dock.home.y - dock.panelY
                x: bx + (0 - bx) * g
                y: by + (0 - by) * g
                width: dock.home.width + (dock.panelWidth - dock.home.width) * g
                height: dock.home.height + (dock.panelHeight - dock.home.height) * g
                opacity: fade
                // 0 18px 48px rgba(0, 0, 0, .45), the 1 px composer border inset.
                BoxShadow {
                    anchors.fill: parent
                    radius: height / 2
                    blur: 48
                    offsetY: 18
                    color: Theme.alpha("black", 0.45 * Theme.shadow)
                }
                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    antialiasing: true
                    color: Theme.composerBg
                    border.width: 1
                    border.color: Theme.composerBorder
                }
            }
            // .dock-row: the icons, 20 px glyphs in 46 × 38 slots.
            Repeater {
                id: icons
                model: dock.items
                Item {
                    id: slot
                    objectName: "modeDockItem-" + modelData.id
                    required property var modelData
                    required property int index
                    readonly property bool focusedHere: dock.focused === index
                    readonly property bool currentHere: dock.current === modelData.id
                    x: dock.pad + index * dock.slotWidth
                    y: dock.pad
                    width: dock.slotWidth
                    height: dock.slotHeight
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.name
                    Accessible.description: modelData.hint
                    Accessible.checkable: true
                    Accessible.checked: currentHere
                    // Where the icon flies from (the button's icons), as seen from its place.
                    readonly property real dx: dock.from.x - (dock.panelX + x + width / 2)
                    readonly property real dy: dock.from.y - (dock.panelY + y + height / 2)
                    // way(): along a curve bent upward, from small and blurred.
                    function way(t, scale0, fadeIn) {
                        const lift = Math.hypot(dx, dy) * 0.3
                        const at = s => {
                            const u = 1 - s
                            return {x: u * u * dx + 2 * u * s * dx * 0.35, y: u * u * dy + 2 * u * s * (dy * 0.2 - lift)}
                        }
                        let p = at(Math.min(1, t))
                        if (t > 1) { // Past the end, on along the last step (WAAPI keyframes).
                            const q = at(15 / 16)
                            p = {x: p.x + (t - 1) * 16 * (p.x - q.x), y: p.y + (t - 1) * 16 * (p.y - q.y)}
                        }
                        const u = 1 - Math.min(1, t)
                        return {x: p.x, y: p.y, scale: scale0 + (1 - scale0) * t,
                                opacity: fadeIn ? Math.min(1, t / 0.3) : 1, blur: fadeIn ? 6 * u * u : 0}
                    }
                    readonly property var flight: {
                        void dock.now
                        if (Theme.reducedMotion || dock.phase === "closed")
                            return {x: 0, y: 0, scale: 1, opacity: 1, blur: 0}
                        if (dock.phase === "closing") {
                            if (currentHere && dock.homing) {
                                // Home whole, landing as the button's own glyph (560 ms, motion).
                                const t = 1 - dock.ease("motion", dock.progress(dock.closingAt, 560))
                                return way(t, dock.origin.width / 20, false)
                            }
                            const t = dock.ease("inn", dock.progress(dock.closingAt, 240))
                            return {x: 0, y: 0, scale: 1 - 0.4 * t, opacity: 1 - t, blur: 4 * t}
                        }
                        const start = dock.openedAt + (dock.flights[index] ?? 0)
                        if (dock.now < start)
                            return {x: dx, y: dy, scale: 0.35, opacity: 0, blur: 6}
                        return way(dock.sampled(dock.land, dock.progress(start, dock.land.duration)), 0.35, true)
                    }
                    PathIcon {
                        id: glyph
                        x: (slot.width - 20) / 2
                        y: (slot.height - 20) / 2
                        width: 20
                        height: 20
                        name: slot.modelData.icon
                        stroke: 4.6 // .dock-glyph svg[viewBox="30 30 60 60"]
                        color: slot.modelData.tone === "warn" ? Theme.warn
                             : slot.focusedHere ? Theme.accent : Theme.muted
                        Behavior on color { ColorAnimation { duration: 250 } }
                        opacity: slot.flight.opacity
                        transform: [
                            Scale { origin.x: 10; origin.y: 10; xScale: slot.flight.scale; yScale: slot.flight.scale },
                            Translate { x: slot.flight.x; y: slot.flight.y }
                        ]
                        layer.enabled: slot.flight.blur > 0.2
                        layer.effect: MultiEffect {
                            blurEnabled: true
                            blurMax: 16
                            autoPaddingEnabled: true
                            blur: Math.min(1, slot.flight.blur / (0.27 * 16))
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onEntered: if (dock.open && !dock.choosing) dock.focusAt(slot.index)
                        onClicked: dock.pick(slot.index, false)
                    }
                }
            }
            // .dock-dot: under the current mode, gliding to a new one
            // (0.7 s on cubic-bezier(.34, 1.25, .64, 1)); in once its icon lands.
            Rectangle {
                id: dot
                objectName: "modeDockDot"
                readonly property int at: dock.indexOf(dock.current)
                visible: at >= 0
                property real place: dock.slotX(Math.max(0, at)) - 2
                Behavior on place {
                    enabled: !Theme.reducedMotion && dock.open
                    NumberAnimation { duration: 700; easing.type: Easing.Bezier; easing.bezierCurve: [0.34, 1.25, 0.64, 1, 1, 1] }
                }
                x: place
                y: dock.pad + dock.slotHeight + 2 - 4
                width: 4
                height: 4
                radius: 2
                readonly property bool warn: (dock.items[Math.max(0, at)] ?? {}).tone === "warn"
                color: warn ? Theme.warn : Theme.alpha(Theme.accent, 0.6)
                readonly property real enter: {
                    void dock.now
                    if (Theme.reducedMotion)
                        return 1
                    // 0 → 1.4 at .55 → 1 (520 ms, out), from dotAt.
                    const t = dock.ease("out", dock.progress(dock.openedAt + dock.dotAt, 520))
                    return t
                }
                readonly property real fall: dock.phase === "closing" ? dock.ease("inn", dock.progress(dock.closingAt, 180)) : 0
                opacity: Math.min(1, enter / 0.55) * (1 - fall)
                scale: enter < 0.55 ? 1.4 * enter / 0.55 : 1.4 - 0.4 * (enter - 0.55) / 0.45
            }
        }
        ShaderEffectSource {
            id: underTexture
            sourceItem: under
            sourceRect: Qt.rect(0, 0, panel.width, panel.height)
            visible: false
            live: lens.opacity > 0
        }

        // The lens over the icon in focus (.dock-lens, liquid-glass.js with
        // zoom 1.1, edge 3.5, band 5), on whole pixels, appearing late with
        // a small overshoot (from .55).
        Item {
            id: lens
            objectName: "modeDockLens"
            readonly property real appear: 0.55 + 0.45 * dock.backOut(dock.reveal)
            readonly property real grow: (1 + 0.1 * Math.max(-0.5, Math.min(1.2, dock.press))) * appear
            readonly property real xScale: grow * (1 + dock.stretch)
            readonly property real yScale: grow * (1 - 0.35 * dock.stretch)
            width: dock.lensSize.width
            height: dock.lensSize.height
            x: Math.round(dock.xAt(dock.pos)) - width / 2
            y: panel.height / 2 - height / 2
            readonly property real fall: dock.phase === "closing" ? dock.ease("inn", dock.progress(dock.closingAt, 180)) : 0
            opacity: Math.max(0, Math.min(1, dock.reveal * 2.5)) * (1 - fall)
            visible: opacity > 0
            transform: Scale {
                origin.x: lens.width / 2
                origin.y: lens.height / 2
                xScale: lens.xScale
                yScale: lens.yScale
            }
            BoxShadow {
                anchors.fill: parent
                radius: height / 2
                blur: 10
                offsetY: 3
                color: Qt.rgba(0, 0, 0, Theme.light ? 0.12 : 0.35) // --glass-drop
            }
            ShaderEffect {
                anchors.fill: parent
                property var backdrop: underTexture
                property size size: Qt.size(lens.width, lens.height)
                property point origin: Qt.point(lens.x, lens.y)
                property point zoom: Qt.point(lens.xScale, lens.yScale)
                property size backdropSize: Qt.size(panel.width, panel.height)
                property real saturation: Theme.light ? 1.4 : 1.6
                property real brightness: Theme.light ? 1.02 : 1.08
                // --glass-fill; thinner over the light theme (.dock-lens).
                property vector4d fillTop: Qt.vector4d(1, 1, 1, Theme.light ? 0.42 : 0.1)
                property vector4d fillMiddle: Qt.vector4d(1, 1, 1, Theme.light ? 0.12 : 0.02)
                property vector4d fillBottom: Qt.vector4d(1, 1, 1, Theme.light ? 0.26 : 0.05)
                property real magnify: 1.1
                property real bend: 3.5
                property real band: 5
                fragmentShader: "qrc:/shaders/glass.frag.qsb"
            }
            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                readonly property real w: lens.width
                readonly property real h: lens.height
                ShapePath { // --glass-rim: 1 px, lit from the top left (155°).
                    strokeColor: "transparent"
                    fillRule: ShapePath.OddEvenFill
                    fillGradient: LinearGradient {
                        x1: 21 - 0.4226 * 24.4; y1: 17 - 0.9063 * 24.4
                        x2: 21 + 0.4226 * 24.4; y2: 17 + 0.9063 * 24.4
                        GradientStop { position: 0; color: Qt.rgba(1, 1, 1, Theme.light ? 1 : 0.7) }
                        GradientStop { position: 0.32; color: Theme.light ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(1, 1, 1, 0.14) }
                        GradientStop { position: 0.62; color: Theme.light ? Qt.rgba(0, 0, 0, 0.06) : Qt.rgba(1, 1, 1, 0.05) }
                        GradientStop { position: 1; color: Qt.rgba(1, 1, 1, Theme.light ? 0.8 : 0.38) }
                    }
                    PathRectangle { x: 0; y: 0; width: 42; height: 34; radius: 17 }
                    PathRectangle { x: 1; y: 1; width: 40; height: 32; radius: 16 }
                }
                ShapePath { // --glass-shade
                    strokeColor: "transparent"
                    fillRule: ShapePath.OddEvenFill
                    fillGradient: LinearGradient {
                        x1: 0; y1: 1; x2: 0; y2: 33
                        GradientStop { position: 0; color: Qt.rgba(1, 1, 1, Theme.light ? 0.9 : 0.22) }
                        GradientStop { position: 0.15; color: "transparent" }
                        GradientStop { position: 0.8; color: "transparent" }
                        GradientStop { position: 1; color: Qt.rgba(0, 0, 0, Theme.light ? 0.06 : 0.25) }
                    }
                    PathRectangle { x: 1; y: 1; width: 40; height: 32; radius: 16 }
                    PathRectangle { x: 1; y: 2; width: 40; height: 29.5; radius: 14.75 }
                }
            }
            // A keyboard focus: a 2 px ring 3 px outside the lens.
            Rectangle {
                objectName: "modeDockFocus"
                anchors.fill: parent
                anchors.margins: -5
                radius: height / 2
                color: "transparent"
                visible: dock.keyboard && dock.open
                border.width: 2
                border.color: Theme.alpha(Theme.strong, 0.35)
            }
        }

        // .dock-title: 14 px in, 16 px above the capsule; the name's box
        // (1.1 em), 10 px, then the line.
        Item {
            id: hintBox
            objectName: "modeDockHint"
            x: 14
            y: -16 - height
            width: 300
            height: hintColumn.tallest
            readonly property real shownWidth: {
                const item = hintRepeater.itemAt(dock.hinted)
                return item ? item.implicitWidth : 0
            }
            Item {
                id: hintColumn
                readonly property real tallest: {
                    let most = 0
                    for (let i = 0; i < hintRepeater.count; ++i) {
                        const item = hintRepeater.itemAt(i)
                        if (item)
                            most = Math.max(most, item.height)
                    }
                    return most
                }
                Repeater {
                    id: hintRepeater
                    model: dock.items
                    // .dock-hint: 13/17 at 500, fg .55 (the warning colour at
                    // .9 for Full); in .48 s after its delay, out in .2 s.
                    Text {
                        id: hint
                        required property var modelData
                        required property int index
                        readonly property bool shownHere: dock.hinted === index && dock.phase === "open"
                        // text-wrap: balance: as narrow as keeps its line count at 300 px.
                        width: balanced
                        property real balanced: 300
                        function balance() {
                            probe.width = 300
                            const lines = probe.lineCount
                            if (lines <= 1) {
                                balanced = Math.min(300, probe.implicitWidth)
                                return
                            }
                            let low = 0, high = 300
                            while (high - low > 0.5) {
                                const middle = (low + high) / 2
                                probe.width = middle
                                if (probe.lineCount > lines)
                                    low = middle
                                else
                                    high = middle
                            }
                            balanced = Math.ceil(high)
                        }
                        Component.onCompleted: balance()
                        onTextChanged: balance()
                        Text {
                            id: probe
                            visible: false
                            text: hint.text
                            font: hint.font
                            wrapMode: Text.WordWrap
                        }
                        wrapMode: Text.WordWrap
                        lineHeightMode: Text.FixedHeight
                        lineHeight: 17
                        text: modelData.hint
                        color: modelData.tone === "warn" ? Theme.alpha(Theme.warn, 0.9) : Theme.alpha(Theme.strong, 0.55)
                        font.pixelSize: 13
                        font.weight: Theme.weight(500)
                        property real shown: 0
                        onShownHereChanged: {
                            hold.stop()
                            showing.stop()
                            if (Theme.reducedMotion) {
                                shown = shownHere ? 1 : 0
                                return
                            }
                            showing.from = shown
                            showing.to = shownHere ? 1 : 0
                            showing.duration = shownHere ? 480 : 200
                            showing.easing.bezierCurve = shownHere ? [0.22, 1, 0.36, 1, 1, 1] : [0.25, 0.1, 0.25, 1, 1, 1]
                            if (shownHere && dock.hintDelay > 0) {
                                hold.interval = dock.hintDelay
                                hold.start()
                            } else {
                                showing.start()
                            }
                        }
                        Timer { id: hold; onTriggered: showing.start() }
                        NumberAnimation { id: showing; target: hint; property: "shown"; easing.type: Easing.Bezier }
                        opacity: shown
                        transform: Translate { y: 5 * (1 - hint.shown) }
                        layer.enabled: shown > 0 && shown < 1
                        layer.effect: MultiEffect {
                            blurEnabled: true
                            blurMax: 16
                            autoPaddingEnabled: true
                            blur: Math.min(1, 3 * (1 - hint.shown) / (0.27 * 16))
                        }
                    }
                }
            }
        }
        Item {
            id: wordBox
            objectName: "modeDockWord"
            readonly property Item newest: {
                void words.count
                for (let i = wordRepeater.count - 1; i >= 0; --i) {
                    const item = wordRepeater.itemAt(i)
                    if (item && item.leftAt < 0)
                        return item
                }
                return null
            }
            x: 14
            y: hintBox.y - 10 - height
            width: 1
            height: 1.1 * dock.size
            Repeater {
                id: wordRepeater
                model: words
                delegate: Word {}
            }
        }
    }

    // .stage-word: weight 560, letter spacing −.022 em, in the accent with
    // a 28 px glow (.22, .08 light). Its letters leave the lens small and
    // blurred, nearest first, are tossed a little past their place and
    // drop in (FLIGHT: 680 ms, ≤ 26 ms apart); an old name melts upward
    // (230 ms, 10 ms apart); closing, the letters dissolve upward, last
    // first (200 ms, ≤ 8 ms apart).
    component Word: Item {
        id: word
        required property string text
        required property real born
        required property real delay
        required property real leftAt
        required property real fx
        required property real fy
        readonly property int count: letters.count
        width: row.implicitWidth
        height: 1.1 * dock.size
        anchors.bottom: parent.bottom
        property var order: []
        property point lensAt: Qt.point(0, 0)
        Component.onCompleted: {
            Theme.settle(row)
            const at = dock.mapToItem(word, fx, fy)
            lensAt = at
            const spots = []
            for (let i = 0; i < letters.count; ++i) {
                const letter = letters.itemAt(i)
                spots.push({i: i, d: Math.hypot(at.x - (letter.x + letter.width / 2), at.y - (letter.y + letter.height / 2))})
            }
            spots.sort((a, b) => a.d - b.d)
            const order = []
            spots.forEach((spot, rank) => { order[spot.i] = rank })
            word.order = order
        }
        TextGlow {
            source: row
            sigma: 14
            color: Theme.glow
            strength: Theme.light ? 0.08 : 0.22
        }
        Row {
            id: row
            Repeater {
                id: letters
                model: Array.from(word.text)
                delegate: Text {
                    id: letter
                    required property int index
                    required property string modelData
                    text: modelData
                    color: Theme.accent
                    height: 1.1 * dock.size
                    verticalAlignment: Text.AlignVCenter
                    font.pointSize: Theme.points(dock.size)
                    font.weight: Font.Medium // 560: Chromium draws the nearest face
                    font.letterSpacing: -0.022 * dock.size
                    readonly property real step: Math.min(26, 240 / Math.max(1, word.count))
                    readonly property real rank: word.order[index] ?? index
                    readonly property real startAt: word.born + word.delay + rank * step
                    readonly property real t: Theme.reducedMotion ? 1 : dock.ease("flight", dock.progress(startAt, 680))
                    readonly property real u: 1 - t
                    readonly property real dx: word.lensAt.x - (x + width / 2)
                    readonly property real dy: word.lensAt.y - (y + height / 2)
                    readonly property real toss: 0.38 * dock.size
                    readonly property real flyX: u * u * dx + 2 * u * t * dx * 0.3
                    readonly property real flyY: u * u * dy - 2 * u * t * toss
                    readonly property real grow: t < 0.75 ? 0.2 + 0.88 * t / 0.75 : 1.08 - 0.08 * (t - 0.75) / 0.25
                    readonly property real spin: (Math.sign(dx) || 1) * 20 * u * u
                    readonly property real melt: Theme.reducedMotion || word.leftAt < 0 ? 0
                        : dock.ease("leave", dock.progress(word.leftAt + index * 10, 230))
                    readonly property real gone: Theme.reducedMotion || dock.closingAt < 0 || word.leftAt >= 0 ? 0
                        : dock.ease("leave", dock.progress(dock.closingAt + (word.count - 1 - index) * Math.min(8, 70 / Math.max(1, word.count)), 200))
                    readonly property real soft: 7 * Math.max(0, 1 - t / 0.7) + 8 * melt + 6 * gone
                    // Still waiting out its delay: hidden (dock.js hold()).
                    opacity: (dock.now < startAt && !Theme.reducedMotion ? 0 : Math.min(1, t / 0.18)) * (1 - melt) * (1 - gone)
                    transform: [
                        Scale {
                            origin.x: letter.width / 2
                            origin.y: letter.height / 2
                            xScale: letter.grow * (1 + 0.06 * letter.melt)
                            yScale: xScale
                        },
                        Rotation { origin.x: letter.width / 2; origin.y: letter.height / 2; angle: letter.spin },
                        Translate {
                            x: letter.flyX
                            y: letter.flyY - dock.size * (0.45 * letter.melt + 0.3 * letter.gone)
                        }
                    ]
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
}
