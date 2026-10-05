import QtQuick
import QtQuick.Effects
import OpenGhost.Native

// The effort's stage (OpenGhost 1.2 effort-stage.js): while the slider is
// open the app frosts over around it, softly, as it does around a selection,
// and the level's name stands over it with a line saying what it does. The
// name comes out of the lens: its letters fly up from the glass and settle
// into place while the old name melts away upward. It carries the effort:
// heavier, tighter and brighter toward the top, straining toward the next
// level while the lens is dragged and leaning the way the lens moves. Every
// level arrives in its own tempo (the lowest all at once, the highest letter
// by letter), and the highest then breathes. Placed in the effort panel's
// coordinates; `lens` is the slider's lens, `backdrop` the window content
// the patch frosts. With reduced motion names and the patch simply appear.
Item {
    id: stage
    required property Item lens
    required property Item backdrop
    // The panel's size: the patch covers it, the name stands 16 px above it.
    property real panelWidth: 264
    property real panelHeight: 48
    // The window width the name's size follows (clamp(32px, 3.4vw, 46px)).
    property real viewWidth: 1280
    // Pressed on the patch: the panel closes.
    signal dismissed()

    // The shown level's index, or -1 while closed.
    property int index: -1
    property real strain: 0
    property real lean: 0
    readonly property real size: Math.max(32, Math.min(46, viewWidth * 0.034))

    readonly property var names: ({ off: "Instant", minimal: "Minimal", low: "Low", medium: "Medium",
                                    high: "High", xhigh: "Extra high", max: "Max" })
    readonly property var hints: ({ off: "Answers right away, without thinking", low: "A quick thought first",
                                    medium: "Thinks it over", high: "Thinks it through",
                                    xhigh: "Thinks longer on hard problems", max: "Thinks as long as it takes" })
    function nameOf(level) { return names[level] ?? level.charAt(0).toUpperCase() + level.slice(1) }
    function hintOf(level) { return hints[level] ?? "" }

    // One clock for the letters, hints and patch, in ms (Theme.clock(), read
    // whenever something starts and on every frame); it runs while anything
    // moves (always, while the top level breathes).
    property real now: 0
    property real busyUntil: 0
    property bool breathing: false
    function keep(until) {
        busyUntil = Math.max(busyUntil, until)
        if (!Theme.reducedMotion && !clock.running)
            clock.start()
    }
    FrameAnimation {
        id: clock
        onTriggered: {
            stage.now = Theme.clock()
            if (stage.now >= stage.busyUntil && !stage.breathing)
                stop()
            stage.sweep()
        }
    }
    readonly property var curves: ({ out: [0.22, 1, 0.36, 1], leave: [0.4, 0, 1, 1], flight: [0.3, 0.6, 0.2, 1],
                                     inn: [0.42, 0, 1, 1], motion: [0.32, 0.72, 0, 1], sway: [0.42, 0, 0.58, 1] })
    function ease(curve, t) {
        const c = curves[curve]
        return t <= 0 ? 0 : t >= 1 ? 1 : Theme.bezier(c[0], c[1], c[2], c[3], t)
    }
    function progress(start, duration) { return Math.max(0, Math.min(1, (now - start) / duration)) }

    // Words and hints on stage: the newest last; older ones leaving.
    ListModel { id: words }
    ListModel { id: lines }
    // The patch: 0 … 1 in 460 ms (motion), out in 320 ms.
    property real veilFrom: 0
    property real veilAt: -1e9
    property real veilGoal: 0
    readonly property real veil: {
        const d = veilGoal > veilFrom ? 460 : 320
        return Theme.reducedMotion ? veilGoal
            : veilFrom + (veilGoal - veilFrom) * ease("motion", progress(veilAt, d))
    }
    property bool instant: true
    property real closedAt: -1

    // The app frosts over around the slider, and the name comes out of the lens.
    function open(at, levels) {
        now = Theme.clock()
        clear()
        closedAt = -1
        show(at, levels, 1, 150, true)
        veilFrom = 0
        veilGoal = 1
        veilAt = now
        keep(now + 460)
    }
    // The letters dissolve upward, last first, and the app comes back.
    function close() {
        now = Theme.clock()
        index = -1
        breathing = false
        if (Theme.reducedMotion) {
            veilGoal = 0
            return
        }
        closedAt = now
        veilFrom = veil
        veilGoal = 0
        veilAt = now
        keep(now + 320)
    }
    // Once the panel is gone, nothing of this visit is left for the next one.
    function clear() {
        words.clear()
        lines.clear()
        index = -1
        breathing = false
        closedAt = -1
        veilFrom = veilGoal = 0
        strain = lean = 0
    }
    // Where the lens is heading picks the name. Answers whether a new name
    // came out, so the lens can give a small start.
    function aim(goal, levels, speed) {
        const max = levels.length - 1, at = Math.max(0, Math.min(max, goal)), to = Math.round(at)
        if (index < 0 || speed > 4.5 || to === index || Math.abs(at - index) < 0.5 + 0.08)
            return false
        show(to, levels, Math.sign(to - index), 0, false)
        return true
    }
    function show(at, levels, dir, delay, instantPatch) {
        const level = levels[at]
        if (level === undefined)
            return
        const top = at === levels.length - 1, heat = levels.length > 1 ? at / (levels.length - 1) : 1
        now = Theme.clock()
        leave()
        index = at
        instant = instantPatch || Theme.reducedMotion
        const chars = Array.from(nameOf(level)).length
        const duration = 440 + 320 * heat, step = 44 * heat
        words.append({ text: nameOf(level), heat: heat, born: now, delay: delay, leftAt: -1,
                       arrive: delay + duration + step * (chars - 1), highest: top })
        swapHint(hintOf(level), dir, delay)
        breathing = top && !Theme.reducedMotion
        keep(now + delay + duration + step * chars)
        Qt.callLater(() => { stage.instant = false })
    }
    function leave() {
        for (let i = 0; i < words.count; ++i)
            if (words.get(i).leftAt < 0)
                words.setProperty(i, "leftAt", now)
        keep(now + 190 + 10 * 24)
    }
    // The line under the name hands over in turn: the old one goes first.
    function swapHint(text, dir, delay) {
        for (let i = 0; i < lines.count; ++i)
            if (lines.get(i).leftAt < 0)
                lines.setProperty(i, "leftAt", now)
        if (text)
            lines.append({ line: text, dir: dir, born: now, delay: delay ? delay + 220 : 110, leftAt: -1 })
        keep(now + (delay ? delay + 220 : 110) + 380)
    }
    // Words and hints whose leaving has finished are dropped.
    function sweep() {
        for (let i = words.count - 1; i >= 0; --i) {
            const left = words.get(i).leftAt
            if (left >= 0 && now > left + 190 + 10 * 24)
                words.remove(i)
        }
        for (let i = lines.count - 1; i >= 0; --i) {
            const left = lines.get(i).leftAt
            if (left >= 0 && now > left + 150)
                lines.remove(i)
        }
    }
    // --strain's 0.3 s transition on the motion curve.
    property real strained: strain
    Behavior on strained { enabled: !Theme.reducedMotion; NumberAnimation { duration: 300; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
    Connections {
        target: Theme
        function onReducedMotionChanged() {
            if (!Theme.reducedMotion)
                return
            clock.stop()
            stage.breathing = false
            for (let i = words.count - 1; i >= 0; --i)
                if (words.get(i).leftAt >= 0)
                    words.remove(i)
            for (let i = lines.count - 1; i >= 0; --i)
                if (lines.get(i).leftAt >= 0)
                    lines.remove(i)
        }
    }

    // The patch behind the name and the slider: the selection's veil
    // (blur 5 px, brightness .78, contrast .949), its edges fading over
    // 64 px, 18 px past the name and the panel at full strength. It follows
    // the name (.5 s) and blooms out of the panel's corner over the button.
    readonly property rect box: {
        let left = 0, top = 0, right = panelWidth, bottom = panelHeight
        const word = wordBox.newest, hint = hintBox.newest
        if (word) {
            left = Math.min(left, wordBox.x + wordBox.width - word.width)
            top = Math.min(top, wordBox.y)
        }
        if (hint) {
            left = Math.min(left, hintBox.x + hintBox.width - hint.width)
            bottom = Math.max(bottom, hintBox.y + hintBox.height)
        }
        const edge = 18 + 64
        return Qt.rect(left - edge, top - edge, right - left + 2 * edge, bottom - top + 2 * edge)
    }
    Item {
        id: patch
        z: -1
        visible: stage.veil > 0.001
        x: stage.box.x
        y: stage.box.y
        width: stage.box.width
        height: stage.box.height
        Behavior on x { enabled: !stage.instant && !Theme.reducedMotion; NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
        Behavior on y { enabled: !stage.instant && !Theme.reducedMotion; NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
        Behavior on width { enabled: !stage.instant && !Theme.reducedMotion; NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
        Behavior on height { enabled: !stage.instant && !Theme.reducedMotion; NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
        readonly property real bloom: 0.45 + 0.55 * stage.veil
        transform: Scale {
            origin.x: stage.panelWidth - patch.x
            origin.y: stage.panelHeight - patch.y
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
            sourceItem: patch.visible ? stage.backdrop : null
            // What lies under the patch as drawn (the bloom scales it).
            sourceRect: {
                void patch.bloom; void patch.x; void patch.y; void patch.width; void patch.height
                void stage.x; void stage.y
                return patch.visible && stage.backdrop
                    ? patch.mapToItem(stage.backdrop, 0, 0, patch.width, patch.height) : Qt.rect(0, 0, 0, 0)
            }
        }
        // blur(5px), growing from 0 with the veil: a Gaussian across, then down.
        ShaderEffect {
            id: veilAcross
            anchors.fill: parent
            property var source: behind
            property vector2d delta: Qt.vector2d(1 / width, 0)
            property real sigma: 5 * stage.veil
            property real goo: 0
            property vector4d tint: Qt.vector4d(0, 0, 0, 0)
            fragmentShader: "qrc:/shaders/blur.frag.qsb"
        }
        ShaderEffectSource { id: veilAcrossTexture; sourceItem: veilAcross; hideSource: true; visible: false }
        ShaderEffect {
            anchors.fill: parent
            property var source: veilAcrossTexture
            property vector2d delta: Qt.vector2d(0, 1 / height)
            property real sigma: 5 * stage.veil
            property real goo: 0
            property vector4d tint: Qt.vector4d(0, 0, 0, 0)
            fragmentShader: "qrc:/shaders/blur.frag.qsb"
        }
        // brightness(.78) contrast(.949): c·.74 + .025, as rgba(25, 25, 25, .26).
        // Light: contrast(.6) brightness(1.25): c·.75 + 64, as white at .25.
        Rectangle {
            anchors.fill: parent
            color: Theme.light ? Qt.rgba(1, 1, 1, 0.25 * stage.veil)
                               : Qt.rgba(25 / 255, 25 / 255, 25 / 255, 0.26 * stage.veil)
        }
        MouseArea {
            anchors.fill: parent
            onPressed: stage.dismissed()
        }
    }
    // .effort-title: right 18 px, 16 px above the panel; the name's box
    // (1.1 em), 10 px, the hint's (16 px).
    Item {
        id: wordBox
        objectName: "effortWord"
        readonly property Item newest: {
            void words.count
            for (let i = repeater.count - 1; i >= 0; --i) {
                const item = repeater.itemAt(i)
                if (item && item.leftAt < 0)
                    return item
            }
            return null
        }
        x: stage.panelWidth - 18 - width
        y: -16 - 16 - 10 - height
        width: 1
        height: 1.1 * stage.size
        // skewX(--lean), from the bottom right.
        transform: Matrix4x4 {
            readonly property real k: Math.tan(stage.lean * Math.PI / 180)
            matrix: Qt.matrix4x4(1, k, 0, -k * wordBox.height, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        }
        Repeater {
            id: repeater
            model: words
            delegate: Word {}
        }
    }
    Item {
        id: hintBox
        readonly property Item newest: {
            void lines.count
            for (let i = hintRepeater.count - 1; i >= 0; --i) {
                const item = hintRepeater.itemAt(i)
                if (item && item.leftAt < 0)
                    return item
            }
            return null
        }
        x: stage.panelWidth - 18 - width
        y: -16 - 16
        width: 1
        height: 16
        Repeater {
            id: hintRepeater
            model: lines
            // .effort-hint: 13 px/16 px 500, rgba(255, 255, 255, .55).
            delegate: Text {
                id: hint
                required property string line
                required property real dir
                required property real born
                required property real delay
                required property real leftAt
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 16
                text: line
                color: Theme.alpha(Theme.strong, 0.55)
                font.pixelSize: 13
                font.weight: Theme.weight(500)
                verticalAlignment: Text.AlignVCenter
                // In: 380 ms (out) from 5 px along its direction, blurred 3 px;
                // out: 150 ms ease-in, 5 px the other way.
                readonly property real rise: Theme.reducedMotion ? 1
                    : stage.ease("out", stage.progress(born + delay, 380))
                readonly property real fall: Theme.reducedMotion || leftAt < 0 ? 0
                    : stage.ease("inn", stage.progress(leftAt, 150))
                readonly property real closing: Theme.reducedMotion || stage.closedAt < 0 ? 0
                    : stage.progress(stage.closedAt, 150)
                opacity: rise * (1 - fall) * (1 - closing)
                transform: Translate { y: hint.dir * 5 * (1 - hint.rise) - hint.dir * 5 * hint.fall }
                layer.enabled: !Theme.reducedMotion && (rise < 1 || fall > 0 || closing > 0)
                layer.effect: MultiEffect {
                    blurEnabled: true
                    blurMax: 16
                    blur: Math.min(1, (3 * (1 - hint.rise) + 3 * hint.fall + 4 * hint.closing) / 4.3)
                }
            }
        }
    }

    // .effort-word: weight 300 + 400·heat, letter spacing −.01 − .025·heat em,
    // a glow of 6 + 30·heat px at .34·heat, right- and bottom-aligned.
    component Word: Item {
        id: word
        required property string text
        required property real heat
        required property real born
        required property real delay
        required property real leftAt
        required property real arrive
        required property bool highest
        readonly property bool live: leftAt < 0
        readonly property real burn: Math.max(0, Math.min(1, heat + (live ? stage.strained : 0)))
        readonly property real spacing: (-0.01 - 0.025 * burn) * stage.size
        readonly property int count: letters.count
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: row.implicitWidth + spacing
        height: 1.1 * stage.size
        // The lens, as seen from each letter's place, and the letters'
        // order out of it: nearest first.
        property point lensAt: Qt.point(0, 0)
        property var order: []
        Component.onCompleted: {
            Theme.settle(row)
            const lens = stage.lens.mapToItem(word, stage.lens.width / 2, stage.lens.height / 2)
            lensAt = lens
            const spots = []
            for (let i = 0; i < letters.count; ++i) {
                const letter = letters.itemAt(i)
                const dx = lens.x - (letter.x + letter.width / 2), dy = lens.y - (letter.y + letter.height / 2)
                spots.push({ i: i, d: Math.hypot(dx, dy) })
            }
            spots.sort((a, b) => a.d - b.d)
            const order = []
            spots.forEach((spot, rank) => { order[spot.i] = rank })
            word.order = order
        }
        // The glow, under the letters as drawn: a Gaussian of σ = radius / 2,
        // as Chromium blurs a text-shadow (MultiEffect's blur is not one), across
        // and down (shaders/blur.frag), in --glow-rgb at --glow (.34, .12 light).
        readonly property real glowSigma: (6 + 30 * burn) / 2
        Loader {
            active: word.burn > 0.001 && word.width > 0
            sourceComponent: Item {
                readonly property real pad: 56
                readonly property size texture: Qt.size(Math.ceil(row.width + 2 * pad), Math.ceil(row.height + 2 * pad))
                ShaderEffectSource {
                    id: ink
                    sourceItem: row
                    sourceRect: Qt.rect(-parent.pad, -parent.pad, row.width + 2 * parent.pad, row.height + 2 * parent.pad)
                    textureSize: parent.texture
                    visible: false
                }
                ShaderEffect {
                    id: across
                    x: -parent.pad
                    y: -parent.pad
                    width: row.width + 2 * parent.pad
                    height: row.height + 2 * parent.pad
                    property var source: ink
                    property vector2d delta: Qt.vector2d(1 / parent.texture.width, 0)
                    property real sigma: word.glowSigma
                    property real goo: 0
                    property vector4d tint: Qt.vector4d(0, 0, 0, 0)
                    fragmentShader: "qrc:/shaders/blur.frag.qsb"
                }
                ShaderEffectSource {
                    id: acrossTexture
                    sourceItem: across
                    hideSource: true
                    textureSize: parent.texture
                    visible: false
                }
                ShaderEffect {
                    x: -parent.pad
                    y: -parent.pad
                    width: row.width + 2 * parent.pad
                    height: row.height + 2 * parent.pad
                    opacity: (Theme.light ? 0.12 : 0.34) * word.burn
                    property var source: acrossTexture
                    property vector2d delta: Qt.vector2d(0, 1 / parent.texture.height)
                    property real sigma: word.glowSigma
                    property real goo: 0
                    property vector4d tint: Qt.vector4d(Theme.glow.r, Theme.glow.g, Theme.glow.b, 1)
                    fragmentShader: "qrc:/shaders/blur.frag.qsb"
                }
            }
        }
        Row {
            id: row
            spacing: word.spacing
            Repeater {
                id: letters
                model: Array.from(word.text)
                delegate: Text {
                    id: letter
                    required property int index
                    required property string modelData
                    text: modelData
                    color: Theme.accent
                    height: 1.1 * stage.size
                    verticalAlignment: Text.AlignVCenter
                    font.pointSize: Theme.points(stage.size)
                    font.weight: Theme.weight(Math.round(300 + 400 * word.burn))
                    // The flight out of the lens (duration 440 … 760 ms and a
                    // stagger of 0 … 44 ms from the lowest level to the highest).
                    readonly property real rank: word.order[index] ?? index
                    readonly property real startAt: word.born + word.delay + rank * 44 * word.heat
                    readonly property real e: Theme.reducedMotion ? 1
                        : stage.ease("flight", stage.progress(startAt, 440 + 320 * word.heat))
                    readonly property real u: 1 - e
                    readonly property real dx: word.lensAt.x - (x + width / 2)
                    readonly property real dy: word.lensAt.y - (y + height / 2)
                    readonly property real toss: 0.38 * stage.size
                    readonly property real flyX: u * u * dx + 2 * u * e * dx * 0.3
                    readonly property real flyY: u * u * dy - 2 * u * e * toss
                    readonly property real grow: e < 0.75 ? 0.2 + 0.88 * e / 0.75 : 1.08 - 0.08 * (e - 0.75) / 0.25
                    readonly property real spin: (Math.sign(dx) || 1) * 26 * u * u
                    // Leaving melts up (190 ms), closing dissolves up (170 ms).
                    readonly property real melt: Theme.reducedMotion || word.leftAt < 0 ? 0
                        : stage.ease("leave", stage.progress(word.leftAt + index * 10, 190))
                    readonly property real gone: Theme.reducedMotion || stage.closedAt < 0 || !word.live ? 0
                        : stage.ease("inn", stage.progress(stage.closedAt + (word.count - 1 - index) * 10, 170))
                    // At the top the name breathes with the Ghost once it has arrived.
                    readonly property real breath: {
                        if (!word.highest || !word.live || Theme.reducedMotion)
                            return 0
                        const t = stage.now - (word.born + word.arrive + index * 150)
                        if (t <= 0)
                            return 0
                        const e = stage.ease("sway", (t % 2600) / 2600)
                        return e < 0.5 ? e * 2 : 2 - e * 2
                    }
                    readonly property real soft: 7 * Math.max(0, 1 - e / 0.7) + 8 * melt + 6 * gone
                    opacity: Math.min(1, e / 0.18) * (1 - melt) * (1 - gone)
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
                            y: letter.flyY - stage.size * (0.45 * letter.melt + 0.3 * letter.gone + 0.07 * letter.breath)
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
