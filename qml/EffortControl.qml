import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import OpenGhost.Cpp

// The effort control (OpenGhost 1.2 effort-button.js, effort-slider.js,
// effort-morph.js, effort-stage.js, effort-paint.js, liquid-glass.js). The
// button shows one segment per level the selected model advertises, lit up
// to the chosen one, in the composer's grey (the accent on hover, focus and
// while open). A click opens the slider above it: the panel grows out of the
// button while its segments stretch into the track, then the glass lens
// appears. The lens follows the pointer on a spring, is pulled toward the
// notches, stretches with speed, rubber-bands past the ends and settles on
// the nearest level (a fling carries it on); it refracts what lies under it
// (shaders/glass.frag). Over the slider the level's name stands on a frosted
// patch (EffortStage). At the highest level lit oil pours over the panel and
// a Ghost looks out of it (shaders/blur.frag and oil.frag). Keys:
// arrows and PageUp/PageDown step, Home/End jump, Enter or Space opens;
// Enter or Escape close. The value is OpenGhost's advertised level name, sent with the
// next run. Hidden, or with reduced motion (even turned on mid-way),
// everything is at its end at once.
Control {
    id: effort
    required property var settings
    // The window content the stage frosts.
    property Item backdrop: null
    readonly property var levels: settings.levels
    readonly property int count: levels.length
    // An absent/unlisted canonical value rests at the first notch without choosing it.
    readonly property int value: Math.max(0, levels.indexOf(settings.thinking))
    property bool locked: false
    readonly property string lockHint: locked ? "You can change effort when OpenGhost finishes" : ""
    readonly property int last: Math.max(0, count - 1)
    // effort-slider.js::setEfforts hides the control when no levels exist.
    // A disabled, empty control still occupied a slot in the composer's Row.
    visible: count > 0
    enabled: count > 0 && !locked
    onCountChanged: if (count === 0) panel.close()
    onLockedChanged: if (locked) panel.close()
    focusPolicy: Qt.TabFocus
    padding: 0
    background: null
    implicitWidth: button.width
    implicitHeight: 34
    Accessible.role: Accessible.Slider
    Accessible.name: levels.includes(settings.thinking) ? "Effort: " + stage.nameOf(settings.thinking) : "Effort"
    Accessible.description: lockHint
    ButtonTip { text: effort.lockHint }
    // Opened or moved from the keyboard: the lens shows the focus ring (focus-visible);
    // closed from it, the button does, until focus leaves or a click.
    property bool keyboardOpened: false
    property bool keyboardClosed: false
    readonly property bool ringed: visualFocus || activeFocus && keyboardClosed

    function choose(index) {
        if (enabled && index >= 0 && index < count && levels[index] !== settings.thinking)
            settings.chooseThinking(levels[index])
    }
    function toggle(keyboard) {
        if (!enabled)
            return
        if (panel.opened) {
            panel.close()
        } else if (!panel.visible) {
            keyboardOpened = keyboard
            panel.open()
        }
    }
    Keys.onPressed: function(event) {
        if (!enabled)
            return
        const steps = {}
        steps[Qt.Key_Left] = steps[Qt.Key_Down] = steps[Qt.Key_PageDown] = -1
        steps[Qt.Key_Right] = steps[Qt.Key_Up] = steps[Qt.Key_PageUp] = 1
        const step = steps[event.key]
        const to = step !== undefined ? Math.max(0, Math.min(last, value + step))
                 : event.key === Qt.Key_Home ? 0 : event.key === Qt.Key_End ? last : -1
        if (to >= 0) {
            event.accepted = true
            // Keys on the open slider make its focus visible, as in Chromium.
            if (panel.visible)
                keyboardOpened = true
            panel.goal = to
            choose(to)
            panel.syncPaint()
            clock.wake()
            // The name heads for the level now: a clock started from an
            // event first ticks a frame late, where 1.2 names it on the
            // first frame after the key.
            clock.steer()
        } else if (panel.visible && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                     || event.key === Qt.Key_Escape)) {
            event.accepted = true
            keyboardClosed = true
            panel.close()
        } else if (!panel.visible && !event.isAutoRepeat && (event.key === Qt.Key_Return
                   || event.key === Qt.Key_Enter || event.key === Qt.Key_Space)) {
            // Pressed as a click presses it; it opens on release.
            event.accepted = true
            keyDown = event.key
        }
    }
    Keys.onReleased: function(event) {
        if (keyDown && event.key === keyDown && !event.isAutoRepeat) {
            event.accepted = true
            keyDown = 0
            if (!panel.visible)
                toggle(true)
        }
    }
    property int keyDown: 0 // The key pressing the button, while held.
    onActiveFocusChanged: {
        if (activeFocus)
            return
        keyDown = 0
        keyboardClosed = false
        if (panel.opened)
            panel.close()
    }

    // effort-button.js: segments 22 × 11 view units, 8 apart, in a 16 px
    // icon; dim .25, .42 on hover or while open; springs press [230, 27],
    // hint [230, 27] and level [500, 45] (the clock's).
    AbstractButton {
        id: button
        objectName: "effortButton"
        height: 34
        width: segments.width + 16
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        readonly property bool lit: panel.visible || effort.enabled && (hovered || effort.ringed)
        property color tone: lit ? Theme.accent : Theme.muted
        Behavior on tone { enabled: !Theme.reducedMotion; ColorAnimation { duration: 200 } }
        onClicked: {
            effort.keyboardClosed = false
            effort.forceActiveFocus()
            effort.toggle(false)
        }
        Spring { id: pressSpring; goal: button.pressed || effort.keyDown ? 1 : 0; k: 230; c: 27 }
        Spring { id: hintSpring; goal: Theme.reducedMotion ? 0 : Math.max(button.hovered ? 1 : 0, panel.visible ? 1 : 0); k: 230; c: 27 }
        // The focus ring: 2 px, white .35, round like the button.
        background: Rectangle {
            radius: height / 2
            color: "transparent"
            visible: effort.ringed && !panel.visible
            border.width: 2
            border.color: Theme.alpha(Theme.strong, 0.35)
        }
        contentItem: Item {
            opacity: effort.enabled ? 1 : 0.3
            Row {
                id: segments
                anchors.centerIn: parent
                anchors.verticalCenterOffset: 2 * pressSpring.value
                spacing: 8 * 16 / 60
                opacity: 1 - 0.3 * pressSpring.value
                // Hidden while open: the morph carries them into the track.
                visible: !panel.visible
                Repeater {
                    id: segmentRepeater
                    objectName: "effortSegments"
                    model: effort.count
                    delegate: Rectangle {
                        required property int index
                        readonly property real dim: 0.25 + (0.42 - 0.25) * hintSpring.value
                        width: 22 * 16 / 60
                        height: 11 * 16 / 60
                        radius: height / 2
                        color: button.tone
                        opacity: dim + (1 - dim) * Math.min(1, Math.max(0, clock.level - index + 1))
                    }
                }
            }
        }
    }

    function smooth(v) { return v * v * (3 - 2 * v) }
    function phase(v, a, b) { return Math.max(0, Math.min(1, (v - a) / (b - a))) }
    function backOut(v) { return 1 + 2.2 * Math.pow(v - 1, 3) + 1.2 * Math.pow(v - 1, 2) }

    Popup {
        id: panel
        objectName: "effortPanel"
        y: -height - 10
        x: button.width - width
        width: 264
        height: 48
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        background: null
        property real goal: effort.value
        property bool dragging: false
        property real grab: 0 // Pointer offset from the lens, in levels.
        // From the open to the close (effort-slider.js opened), while the
        // popup also shows the morph back.
        property bool showing: false
        // The oil at the highest level (effort-paint.js): t pours in over
        // 1.25 s and drains in 0.75 s; bloom (0.8 s, from the panel's
        // landing) spreads it to its reach and brings the Ghost.
        property real t: 0
        property real target: 0
        property real bloom: 1
        property bool blooming: false
        property bool painted: false
        // --paint: the track, fill, notches and lens turn toward the oil.
        readonly property real paint: effort.smooth(effort.phase(t, 0.3, 0.9))
        // --paint-fade: late in the opening, early in the closing.
        readonly property real paintFade: showing ? effort.smooth(effort.phase(shown, 0.92, 0.96))
                                                  : effort.smooth(effort.phase(shown, 0.55, 1))
        readonly property bool alive: target === 1 && !Theme.reducedMotion
        // The morph (effort-morph.js): 0 at the button, 1 open, on a spring.
        readonly property real shown: clock.m
        // The button's place, in the panel's coordinates.
        readonly property rect from: Qt.rect(width - button.width, height + 10, button.width, 34)
        function paintOn(on) {
            target = on ? 1 : 0
            if (on)
                bloom = 1
            if (Theme.reducedMotion)
                t = target
            oilClock.wake()
        }
        // Painted within 0.4 of the top, unpainted only past 0.6 below it,
        // and only on the open, settled panel.
        function syncPaint() {
            if (!showing || clock.mGoal !== 1 || !clock.mDone)
                return
            const level = dragging ? goal : effort.value
            const on = level > effort.last - (painted ? 0.6 : 0.4)
            if (on !== painted) {
                painted = on
                paintOn(on)
            }
        }
        function bloomIn() {
            blooming = true
            if (Theme.reducedMotion)
                bloom = 1
            oilClock.wake()
        }
        onAboutToShow: {
            showing = true
            clock.pos = clock.goalPos = effort.value
            clock.vel = 0
            clock.stretch = clock.stretchVel = clock.press = clock.pressVel = 0
            clock.m = clock.mVel = 0
            clock.mDone = clock.landed = false
            // The oil: full but not yet spread when opened at the top, else none.
            oil.layout()
            painted = effort.value === effort.last
            blooming = false
            target = t = painted ? 1 : 0
            if (painted)
                bloom = 0
            // The segments as the button shows them now, and the level they reach.
            morph.level = effort.value
            const tones = []
            for (let i = 0; i < segmentRepeater.count; ++i) {
                const segment = segmentRepeater.itemAt(i)
                tones.push(Qt.rgba(button.tone.r, button.tone.g, button.tone.b,
                                   segment.opacity * segments.opacity))
            }
            morph.tones = tones
            clock.mGoal = 1
            clock.wake()
        }
        onOpened: stage.open(effort.value, effort.levels)
        onAboutToHide: showing = false
        onClosed: {
            clock.mGoal = 0
            clock.m = clock.mVel = 0
            clock.mDone = true
            stage.clear()
            effort.keyboardOpened = false
            oilClock.stop()
            target = t = 0
            painted = blooming = false
        }
        enter: Transition {}
        // The morph back into the button, then the popup goes.
        exit: Transition {
            SequentialAnimation {
                ScriptAction {
                    script: {
                        panel.dragging = false
                        stage.close()
                        clock.mGoal = 0
                        clock.mDone = clock.landed = false
                        clock.wake()
                    }
                }
                PauseAnimation { duration: Theme.reducedMotion ? 0 : 400 }
            }
        }
        // effort-paint.js tick(): t at its pace, bloom, and the oil's own time
        // (its edges roll while it stands at the top).
        FrameAnimation {
            id: oilClock
            property real time: 0
            running: false
            function wake() {
                if (Theme.reducedMotion || !panel.visible)
                    stop()
                else if (!running && (panel.t !== panel.target || panel.alive))
                    start()
            }
            onTriggered: {
                const dt = Math.max(0, Math.min(frameTime, 0.05))
                time += dt
                panel.t = panel.target > panel.t ? Math.min(panel.target, panel.t + dt / 1.25)
                                                 : Math.max(panel.target, panel.t - dt / 0.75)
                if (panel.blooming)
                    panel.bloom = Math.min(1, panel.bloom + dt / 0.8)
                if (panel.t === panel.target && !panel.alive)
                    stop()
            }
        }
        contentItem: Item {
            id: content
            // The spring's slow tail lands with zero speed at .96 (LAND).
            function land(m) {
                if (m <= 0.8)
                    return Math.max(0, Math.min(1, m * 2 / 1.76))
                return 1 - 2 / 1.76 * Math.pow(Math.max(0, 0.96 - m), 2) / (2 * 0.16)
            }
            // CSS color-mix(in srgb, a, b t): premultiplied, as Chromium mixes.
            function mix(a, b, t) {
                const alpha = a.a + (b.a - a.a) * t
                if (alpha <= 0)
                    return Qt.rgba(0, 0, 0, 0)
                const c = (x, y) => (x * a.a * (1 - t) + y * b.a * t) / alpha
                return Qt.rgba(c(a.r, b.r), c(a.g, b.g), c(a.b, b.b), alpha)
            }
            readonly property real g: land(panel.shown)
            readonly property real reveal: effort.smooth(effort.phase(panel.shown, 0.92, 0.96))

            EffortStage {
                id: stage
                lens: lens
                backdrop: effort.backdrop
                panelWidth: panel.width
                panelHeight: panel.height
                viewWidth: Window.width
                onDismissed: panel.close()
            }
            // Everything the lens looks through, in the panel's order.
            Item {
                id: under
                width: panel.width
                height: panel.height
                // .effort-shell: grows from the button to the panel, fading in
                // over the first 35 %; 0 18px 48px rgba(0, 0, 0, .45) and the
                // composer's 1 px border. 1.2 never tints it (--shell-paint 0):
                // the oil covers it.
                Item {
                    id: shell
                    readonly property rect b: panel.from
                    x: b.x * (1 - content.g)
                    y: b.y * (1 - content.g)
                    width: b.width + (panel.width - b.width) * content.g
                    height: b.height + (panel.height - b.height) * content.g
                    opacity: effort.smooth(effort.phase(panel.shown, 0, 0.35))
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
                // The oil (effort-paint.js): white blobs, a body, a flowing
                // front, a tongue, a drip and rolling puffs along its edges,
                // drawn 40 px and 18 px past the panel. They are blurred by 5 px
                // and cut at alpha .41 (the goo), then lit as a relief: the
                // accent from the top left, white gloss.
                Item {
                    id: oil
                    readonly property real r: panel.height / 2
                    readonly property real gx: 14 + slider.at(effort.last) + 18 + 6 + 17 / 2
                    readonly property real end: gx + 17 / 2 + 13
                    readonly property real source: Math.min(gx, end - r)
                    readonly property real time: oilClock.time
                    readonly property real drop: (r + 1) * Math.max(0, effort.backOut(effort.phase(panel.t, 0, 0.3)))
                    readonly property real flood: effort.phase(panel.t, 0.22, 1)
                    readonly property real spread: effort.smooth(flood)
                    readonly property real wave: Math.sin(Math.PI * flood)
                    readonly property real absorb: 1 - effort.smooth(effort.phase(flood, 0.65, 1))
                    readonly property real front: source - (source - r + 1) * spread
                    readonly property real tail: panel.width - r + (end - panel.width) * effort.smooth(effort.phase(panel.bloom, 0, 0.6))
                    readonly property real flow: effort.phase(panel.t, 0.1, 0.65)
                    readonly property real reach: {
                        const settle = end - r * 0.8 - gx
                        return settle * flow + (end - gx + 14 - settle) * Math.pow(Math.sin(Math.PI * flow), 1.2)
                    }
                    property var puffs: []
                    // The puffs: along the top and bottom about every 32 px, and
                    // two at each end, each with its own size, phases and speeds.
                    function layout() {
                        const random = (a, b) => a + Math.random() * (b - a)
                        const tau = 2 * Math.PI, h = panel.height
                        const rows = {
                            top: { radius: [12, 16], shift: 0, amp: 1 },
                            bottom: { radius: [8, 11], shift: Math.PI, amp: 0.5 },
                            ends: { radius: [12, 14], shift: Math.PI / 2, amp: 0.8 }
                        }
                        const count = Math.max(3, Math.round((end - 2 * r) / 32))
                        const spots = []
                        for (let i = 0; i < count; ++i) {
                            const x = r + (end - 2 * r) * (i + 0.5) / count
                            spots.push([x + random(-4, 4), 8, rows.top], [x + random(-6, 6), h - 7, rows.bottom])
                        }
                        for (const x of [10, end - 10])
                            spots.push([x, r - 7, rows.ends], [x, r + 7, rows.ends])
                        puffs = spots.map(([x, y, row]) => ({
                            x: x, y: y, r: random(row.radius[0], row.radius[1]), shift: row.shift, amp: row.amp,
                            phase: [random(0, tau), random(0, tau), random(0, tau)],
                            speed: [random(0.7, 1.4), random(0.7, 1.4), random(0.7, 1.4)]
                        }))
                    }
                    x: -40
                    y: -18
                    width: end + 80
                    height: panel.height + 36
                    visible: panel.t > 0
                    opacity: panel.paintFade
                    component Blob: Rectangle {
                        property real cx
                        property real cy
                        property real size
                        x: cx + 40 - width / 2
                        y: cy + 18 - height / 2
                        width: 2 * Math.max(0, size)
                        height: width
                        radius: width / 2
                        color: "white"
                    }
                    Item {
                        id: blobs
                        anchors.fill: parent
                        Rectangle { // The body.
                            x: oil.front - oil.drop + 40
                            y: oil.r - oil.drop + 18
                            width: Math.max(0, oil.tail - oil.front + 2 * oil.drop)
                            height: 2 * oil.drop
                            radius: oil.drop
                            color: "white"
                        }
                        Repeater { // The front, flowing left.
                            model: [{ y: 0.22, r: 0.62, lead: 10, phase: 0 }, { y: 0.5, r: 0.7, lead: 18, phase: 1.7 },
                                    { y: 0.78, r: 0.6, lead: 8, phase: 3.1 }, { y: 0.36, r: 0.5, lead: 14, phase: 4.4 }]
                            delegate: Blob {
                                required property var modelData
                                cx: oil.front - modelData.lead * oil.wave + Math.sin(oil.time * 1.9 + modelData.phase) * 3 * oil.wave
                                cy: oil.r + (modelData.y - 0.5) * panel.height * (oil.drop / oil.r)
                                    + Math.sin(oil.time * 2.3 + modelData.phase) * 2 * oil.wave
                                size: modelData.r * oil.drop * oil.absorb
                            }
                        }
                        Blob { // The tongue past the Ghost.
                            cx: oil.gx + oil.reach
                            cy: oil.r + 3
                            size: oil.r * 0.6 * effort.smooth(effort.phase(oil.flow, 0, 0.2))
                                  * effort.smooth(effort.phase(panel.bloom, 0, 0.6))
                        }
                        Blob { // The drip under it.
                            cx: oil.gx + oil.reach * 0.75 - 4
                            cy: panel.height - 4 + 10 * Math.sin(Math.PI * oil.flow)
                            size: oil.r * 0.34 * effort.smooth(effort.phase(oil.flow, 0.1, 0.35))
                                  * (1 - effort.smooth(effort.phase(oil.flow, 0.75, 1)))
                        }
                        Repeater {
                            model: oil.puffs
                            delegate: Blob {
                                required property var modelData
                                readonly property var puff: modelData
                                readonly property real arrival: Math.max(0, Math.min(1, (oil.source - puff.x) / (oil.source - oil.r + 1))) * 0.8
                                readonly property real late: (1 - puff.x / oil.end) * 0.5
                                readonly property real presence: effort.smooth(Math.max(0, Math.min(1, (oil.spread - arrival + 0.1) / 0.3)))
                                    * Math.min(1, oil.drop / oil.r) * effort.smooth(effort.phase(panel.bloom, late, late + 0.5))
                                readonly property real roll: Math.sin(oil.time * 1.1 - puff.x / 140 * 2 * Math.PI + puff.shift)
                                cx: puff.x + Math.sin(oil.time * puff.speed[0] + puff.phase[0]) * 4
                                cy: puff.y + Math.sin(oil.time * puff.speed[1] + puff.phase[1]) * 1.6 * puff.amp
                                size: presence * (puff.r + (roll * 3 + Math.sin(oil.time * puff.speed[2] + puff.phase[2]) * 1.8) * puff.amp)
                            }
                        }
                    }
                    // The filter, pass by pass: blur across and down (5 px) with
                    // the goo's cut, the relief's blur across (1.2 px), then the
                    // light, which blurs the relief down itself.
                    ShaderEffectSource { id: blobsTexture; sourceItem: blobs; hideSource: true; visible: false }
                    ShaderEffect {
                        id: across
                        anchors.fill: parent
                        property var source: blobsTexture
                        property vector2d delta: Qt.vector2d(1 / width, 0)
                        property real sigma: 5
                        property real goo: 0
                        property vector4d tint: Qt.vector4d(0, 0, 0, 0)
                        fragmentShader: "qrc:/shaders/blur.frag.qsb"
                    }
                    ShaderEffectSource {
                        id: acrossTexture
                        sourceItem: across
                        hideSource: true
                        visible: false
                        format: ShaderEffectSource.RGBA16F
                    }
                    ShaderEffect {
                        id: gooPass
                        anchors.fill: parent
                        property var source: acrossTexture
                        property vector2d delta: Qt.vector2d(0, 1 / height)
                        property real sigma: 5
                        property real goo: 1
                        property vector4d tint: Qt.vector4d(0, 0, 0, 0)
                        fragmentShader: "qrc:/shaders/blur.frag.qsb"
                    }
                    ShaderEffectSource {
                        id: gooTexture
                        sourceItem: gooPass
                        hideSource: true
                        visible: false
                        format: ShaderEffectSource.RGBA16F
                    }
                    ShaderEffect {
                        id: reliefPass
                        anchors.fill: parent
                        property var source: gooTexture
                        property vector2d delta: Qt.vector2d(1 / width, 0)
                        property real sigma: 1.2
                        property real goo: 0
                        property vector4d tint: Qt.vector4d(0, 0, 0, 0)
                        fragmentShader: "qrc:/shaders/blur.frag.qsb"
                    }
                    ShaderEffectSource {
                        id: reliefTexture
                        sourceItem: reliefPass
                        hideSource: true
                        visible: false
                        format: ShaderEffectSource.RGBA16F
                    }
                    ShaderEffect {
                        objectName: "effortOil"
                        anchors.fill: parent
                        property var goo: gooTexture
                        property var relief: reliefTexture
                        property vector2d pixel: Qt.vector2d(1 / width, 1 / height)
                        property vector4d tint: Qt.vector4d(Theme.accent.r, Theme.accent.g, Theme.accent.b, 1)
                        fragmentShader: "qrc:/shaders/oil.frag.qsb"
                    }
                }
                // .effort-morph: each button segment stretches into its part of
                // the track, fading under the track as it appears.
                Item {
                    id: morph
                    anchors.fill: parent
                    visible: content.reveal < 1
                    property int level: 0
                    property var tones: []
                    readonly property real trackY: 6 + 18
                    readonly property real segment: 22 * 16 / 60
                    function stop(i) { return 36 + 192 * i / Math.max(1, effort.count - 1) }
                    Repeater {
                        model: effort.count
                        delegate: Rectangle {
                            required property int index
                            readonly property real g: content.g
                            readonly property real fromLeft: panel.from.x + (panel.from.width - segments.width) / 2
                                                             + index * (segments.spacing + morph.segment)
                            readonly property real fromRight: fromLeft + morph.segment
                            readonly property real toLeft: index === 0 ? morph.stop(0) - 3
                                : index === 1 ? morph.stop(0) + 3 : morph.stop(index - 1)
                            readonly property real toRight: index === 0 || index === effort.last
                                ? morph.stop(index) + 3 : morph.stop(index)
                            readonly property real thick: 11 * 16 / 60 + (6 - 11 * 16 / 60) * g
                            readonly property color start: morph.tones[index] ?? Theme.muted
                            readonly property color end: index <= morph.level ? Theme.accent
                                                                               : Theme.alpha(Theme.strong, 0.1)
                            readonly property real alpha: start.a + (end.a - start.a) * g
                            x: fromLeft + (toLeft - fromLeft) * g
                            width: Math.max(thick, fromRight + (toRight - fromRight) * g - x)
                            height: thick
                            y: panel.from.y + 17 + (morph.trackY - panel.from.y - 17) * g - thick / 2
                            radius: thick / 2
                            // Under the fading-in track their alpha keeps the sum the track's own.
                            color: Qt.rgba(start.r + (end.r - start.r) * g, start.g + (end.g - start.g) * g,
                                           start.b + (end.b - start.b) * g,
                                           content.reveal < 1 ? alpha * (1 - content.reveal) / (1 - content.reveal * alpha) : 0)
                        }
                    }
                }
                Item { // The track, its fill cut at the lens, and the notches.
                    id: track
                    x: slider.x
                    y: slider.y
                    width: slider.width
                    height: slider.height
                    opacity: content.reveal
                    Rectangle {
                        x: slider.inset - 3
                        y: slider.height / 2 - 3
                        width: slider.trackWidth + 6
                        height: 6
                        radius: 3
                        color: content.mix(Theme.alpha(Theme.strong, 0.1), Theme.alpha(Theme.paintShade, 0.1), panel.paint)
                        Rectangle {
                            width: Math.max(0, Math.min(parent.width, lens.x + 18 - slider.inset + 3))
                            height: parent.height
                            radius: 3
                            color: Qt.tint(Theme.accent, Theme.alpha(Theme.onAccent, panel.paint))
                        }
                    }
                    Repeater { // Inner notches, lit once the lens passes them.
                        objectName: "effortNotches"
                        model: Math.max(0, effort.count - 2)
                        delegate: Rectangle {
                            required property int index
                            objectName: "effortNotch"
                            readonly property real under: Math.max(0, Math.min(1, (clock.pos - index - 1) * 6 + 0.5))
                            x: slider.at(index + 1) - 2
                            y: slider.height / 2 - 2
                            width: 4
                            height: 4
                            radius: 2
                            // --bare and --covered, each mixed toward the paint.
                            readonly property color bare: content.mix(Theme.alpha(Theme.strong, 0.3),
                                                                      Theme.alpha(Theme.paintShade, 0.22), panel.paint)
                            readonly property color covered: content.mix(Theme.alpha(Theme.onAccent, 0.45),
                                                                         Theme.accent, panel.paint)
                            color: content.mix(bare, covered, under)
                        }
                    }
                }
            }
            ShaderEffectSource {
                id: underTexture
                sourceItem: under
                sourceRect: Qt.rect(0, 0, panel.width, panel.height)
                visible: false
                live: lens.opacity > 0
            }
            // .effort-slider: 36 px, padding 6 14; track inset 22 px.
            Item {
                id: slider
                objectName: "effortSlider"
                x: 14
                y: 6
                width: panel.width - 28
                height: 36
                readonly property real inset: 22
                readonly property real trackWidth: width - 2 * inset
                function at(pos) { return inset + pos / (effort.last || 1) * trackWidth }
                function toPos(px) { return (px - inset) / trackWidth * (effort.last || 1) }
                // The lens (liquid-glass.js, .glass-lens): what lies under it,
                // magnified 1.15 × and bent at its rim, through --glass-backdrop,
                // under --glass-fill; a rim lit from the top left, inner shade, a
                // 0 3px 10px drop and, painted, a .5 px outline. It appears late
                // in the morph with a small overshoot (from .55).
                Item {
                    id: lens
                    objectName: "effortLens"
                    readonly property real appear: {
                        const v = Math.max(0, Math.min(1, (panel.shown - 0.88) / 0.12))
                        return 0.55 + 0.45 * (1 + 2.2 * Math.pow(v - 1, 3) + 1.2 * Math.pow(v - 1, 2))
                    }
                    readonly property real grow: (1 + 0.1 * Math.max(0, Math.min(1.2, clock.press))) * appear
                    readonly property real xScale: grow * (1 + clock.stretch)
                    readonly property real yScale: grow * (1 - 0.35 * clock.stretch)
                    width: 36
                    height: 24
                    // On whole pixels, as the glass reads its backdrop.
                    x: Math.round(slider.at(clock.pos) - width / 2)
                    y: slider.height / 2 - height / 2
                    opacity: effort.smooth(effort.phase(panel.shown, 0.88, 0.96))
                    transform: Scale {
                        origin.x: 18
                        origin.y: 12
                        xScale: lens.xScale
                        yScale: lens.yScale
                    }
                    BoxShadow {
                        anchors.fill: parent
                        radius: 12
                        blur: 10
                        offsetY: 3
                        color: Qt.rgba(0, 0, 0, Theme.light ? 0.12 : 0.35) // --glass-drop
                    }
                    Rectangle { // 0 0 0 .5px rgba(0, 0, 0, --paint × .18 × --shadow)
                        anchors.fill: parent
                        anchors.margins: -0.5
                        radius: height / 2
                        antialiasing: true
                        color: Qt.rgba(0, 0, 0, panel.paint * 0.18 * Theme.shadow)
                        visible: panel.paint > 0
                    }
                    ShaderEffect {
                        objectName: "effortGlass"
                        anchors.fill: parent
                        property var backdrop: underTexture
                        property size size: Qt.size(36, 24)
                        property point origin: Qt.point(slider.x + lens.x, slider.y + lens.y)
                        property point zoom: Qt.point(lens.xScale, lens.yScale)
                        property size backdropSize: Qt.size(panel.width, panel.height)
                        // --glass-backdrop: saturate() brightness()
                        property real saturation: Theme.light ? 1.4 : 1.6
                        property real brightness: Theme.light ? 1.02 : 1.08
                        // --glass-fill
                        property vector4d fillTop: Qt.vector4d(1, 1, 1, Theme.light ? 0.85 : 0.1)
                        property vector4d fillMiddle: Qt.vector4d(1, 1, 1, Theme.light ? 0.55 : 0.02)
                        property vector4d fillBottom: Qt.vector4d(1, 1, 1, Theme.light ? 0.7 : 0.05)
                        // liquid-glass.js's defaults: zoom 1.15, edge 6, band 7.
                        property real magnify: 1.15
                        property real bend: 6
                        property real band: 7
                        fragmentShader: "qrc:/shaders/glass.frag.qsb"
                    }
                    Shape {
                        anchors.fill: parent
                        preferredRendererType: Shape.CurveRenderer
                        ShapePath { // The rim: 1 px, lit from the top left (155°).
                            strokeColor: "transparent"
                            fillRule: ShapePath.OddEvenFill
                            fillGradient: LinearGradient {
                                x1: 18 - 0.4226 * 18.48; y1: 12 - 0.9063 * 18.48
                                x2: 18 + 0.4226 * 18.48; y2: 12 + 0.9063 * 18.48
                                // --glass-rim
                                GradientStop { position: 0; color: Qt.rgba(1, 1, 1, Theme.light ? 1 : 0.7) }
                                GradientStop { position: 0.32; color: Theme.light ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(1, 1, 1, 0.14) }
                                GradientStop { position: 0.62; color: Theme.light ? Qt.rgba(0, 0, 0, 0.06) : Qt.rgba(1, 1, 1, 0.05) }
                                GradientStop { position: 1; color: Qt.rgba(1, 1, 1, Theme.light ? 0.8 : 0.38) }
                            }
                            PathRectangle { x: 0; y: 0; width: 36; height: 24; radius: 12 }
                            PathRectangle { x: 1; y: 1; width: 34; height: 22; radius: 11 }
                        }
                        ShapePath { // --glass-shade: inset 0 1px 1px white .22, inset 0 -1px 2px black .25
                                    // (.9 and .06 in the light theme)
                            strokeColor: "transparent"
                            fillRule: ShapePath.OddEvenFill
                            fillGradient: LinearGradient {
                                x1: 0; y1: 1; x2: 0; y2: 23
                                GradientStop { position: 0; color: Qt.rgba(1, 1, 1, Theme.light ? 0.9 : 0.22) }
                                GradientStop { position: 0.2; color: "transparent" }
                                GradientStop { position: 0.75; color: "transparent" }
                                GradientStop { position: 1; color: Qt.rgba(0, 0, 0, Theme.light ? 0.06 : 0.25) }
                            }
                            PathRectangle { x: 1; y: 1; width: 34; height: 22; radius: 11 }
                            PathRectangle { x: 1; y: 2; width: 34; height: 19.5; radius: 9.75 }
                        }
                    }
                    // Keyboard focus: a 2 px ring 3 px outside.
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -5
                        radius: height / 2
                        color: "transparent"
                        visible: effort.keyboardOpened && panel.opened
                        border.width: 2
                        border.color: Theme.alpha(Theme.strong, 0.35)
                    }
                }
                // The Ghost in the oil (.effort-ghost): it pops in as the oil
                // pours and once the panel has landed.
                Ghost {
                    x: oil.gx - slider.x - width / 2
                    y: slider.height / 2 - height / 2
                    width: 17
                    height: 17 * 70 / 64
                    scale: Math.max(0, effort.backOut(effort.phase(panel.t, 0.08, 0.38))
                                       * effort.backOut(effort.phase(panel.bloom, 0.25, 0.75)))
                    opacity: effort.phase(panel.t, 0.05, 0.25) * effort.smooth(effort.phase(panel.bloom, 0.25, 0.6))
                             * panel.paintFade
                    visible: panel.t > 0 && opacity > 0
                    running: panel.visible && visible
                    reducedMotion: Theme.reducedMotion
                    color: Theme.onAccent
                    eyeColor: Theme.accent
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: effort.enabled
                    preventStealing: true
                    // A drag follows its goal.
                    cursorShape: panel.dragging ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                    // effort-slider.js shape(): magnet pull near notches, rubber past the ends.
                    function shape(raw) {
                        const rubber = d => (1 - 1 / (d * 0.55 / 0.16 + 1)) * 0.16
                        if (raw < 0)
                            return -rubber(-raw)
                        if (raw > effort.last)
                            return effort.last + rubber(raw - effort.last)
                        const d = raw - Math.round(raw)
                        const t = Math.max(0, Math.min(1, Math.abs(d) / 0.22))
                        return raw - d * 0.45 * (1 - t * t * (3 - 2 * t))
                    }
                    onPressed: mouse => {
                        const raw = slider.toPos(mouse.x)
                        const onLens = Math.abs(raw - clock.pos) / (effort.last || 1) * slider.trackWidth <= 18 + 6
                        panel.grab = onLens ? raw - clock.pos : 0
                        panel.dragging = true
                        panel.goal = shape(raw - panel.grab)
                        clock.wake()
                    }
                    onPositionChanged: mouse => {
                        if (!panel.dragging)
                            return
                        panel.goal = shape(slider.toPos(mouse.x) - panel.grab)
                        panel.syncPaint()
                        clock.wake()
                    }
                    onReleased: {
                        panel.dragging = false
                        // A fling carries the lens on: 0.06 s of its speed.
                        const to = Math.round(Math.max(0, Math.min(effort.last, panel.goal + clock.vel * 0.06)))
                        panel.goal = to
                        effort.choose(to)
                        panel.syncPaint()
                        clock.wake()
                    }
                    onCanceled: panel.dragging = false
                }
            }
        }
    }
    onVisibleChanged: if (!visible) clock.settle()
    Connections {
        target: Theme
        function onReducedMotionChanged() {
            if (!Theme.reducedMotion)
                return
            clock.settle()
            oilClock.stop()
            panel.t = panel.target
            if (panel.blooming)
                panel.bloom = 1
        }
    }
    onValueChanged: {
        if (!panel.dragging)
            panel.goal = value
        clock.wake()
    }
    // Settings notifies every change at once; only a new list of levels
    // renames the stage (effort-slider.js setEfforts), never a choice.
    property var shownLevels: []
    onLevelsChanged: {
        const same = levels.length === shownLevels.length && levels.every((level, i) => level === shownLevels[i])
        shownLevels = levels
        if (!same && panel.opened)
            stage.show(Math.max(0, value), levels, 1, 0, false)
    }

    // The springs (effort-slider.js FOLLOW/SETTLE/STRETCH/PRESS, the
    // button's level and effort-morph.js's), stepped at 4 ms each frame,
    // only while moving.
    FrameAnimation {
        id: clock
        property real pos: effort.value
        property real vel: 0
        property real goalPos: effort.value
        property real stretch: 0
        property real stretchVel: 0
        property real press: 0
        property real pressVel: 0
        property real level: effort.value
        property real levelVel: 0
        property real m: 0
        property real mVel: 0
        property real mGoal: 0
        // The morph at rest on its goal, and past its landing (.96) opening.
        property bool mDone: true
        property bool landed: false
        running: false
        function wake() {
            if (Theme.reducedMotion || !effort.visible)
                settle()
            else if (!running)
                start()
        }
        // Every spring at rest on its goal, and the clock stopped.
        function settle() {
            stop()
            pos = panel.goal
            vel = stretch = stretchVel = pressVel = levelVel = mVel = 0
            press = panel.dragging ? 1 : 0
            level = effort.value
            m = mGoal
            morphed()
            steer()
        }
        // effort-morph.js: landed, the oil blooms; at rest open, the paint
        // follows the level.
        function morphed() {
            if (!mDone && Math.abs(mGoal - m) < 0.001 && Math.abs(mVel) < 0.01) {
                m = mGoal
                mVel = 0
                mDone = true
            }
            if (mGoal === 1 && !landed && m >= 0.96) {
                landed = true
                panel.bloomIn()
            }
            if (mDone && mGoal === 1)
                panel.syncPaint()
        }
        // The stage follows the lens: the name it heads for, the strain of a
        // drag toward the next one and the lean of its speed. A new name
        // gives the lens a small start.
        function steer() {
            if (!panel.opened || stage.index < 0)
                return
            stage.strain = panel.dragging ? Math.max(-0.5, Math.min(0.5, pos - stage.index)) / (effort.last || 1) : 0
            stage.lean = Math.max(-6, Math.min(6, -vel * 1.2))
            if (stage.aim(panel.goal, effort.levels, panel.dragging ? Math.abs(vel) : 0)
                && !Theme.reducedMotion) {
                pressVel += 12
                if (!running)
                    start()
            }
        }
        onTriggered: {
            const dt = Math.min(frameTime, 0.032)
            const steps = Math.max(1, Math.ceil(dt / 0.004)), h = dt / steps
            const k = panel.dragging ? 1400 : 320, c = panel.dragging ? 75 : 26
            const pressGoal = panel.dragging ? 1 : 0
            for (let n = 0; n < steps; ++n) {
                vel += ((panel.goal - pos) * k - vel * c) * h
                pos += vel * h
                const stretchGoal = Math.min(0.3, Math.abs(vel) * 0.035)
                stretchVel += ((stretchGoal - stretch) * 520 - stretchVel * 34) * h
                stretch += stretchVel * h
                pressVel += ((pressGoal - press) * 420 - pressVel * 30) * h
                press += pressVel * h
                levelVel += ((effort.value - level) * 500 - levelVel * 45) * h
                level += levelVel * h
                if (!mDone) {
                    mVel += ((mGoal - m) * 420 - mVel * 37) * h
                    m += mVel * h
                }
            }
            morphed()
            steer()
            const moving = panel.dragging || Math.abs(panel.goal - pos) > 0.0005 || Math.abs(vel) > 0.002
                || Math.abs(stretch) > 0.001 || Math.abs(stretchVel) > 0.005
                || Math.abs(pressGoal - press) > 0.001 || Math.abs(pressVel) > 0.005
                || Math.abs(effort.value - level) > 0.001 || Math.abs(levelVel) > 0.005
                || !mDone
            if (!moving)
                settle()
        }
    }
}
