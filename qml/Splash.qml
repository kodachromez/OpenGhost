import QtQuick
import QtQuick.Shapes
import OpenGhost.Native

// The startup splash, OpenGhost 1.3's (splash.js, splash-mist.js). Night
// falls over the window and mist drifts through it, with motes of light in
// the air (shaders/mist.frag). The Ghost comes out of the mist far off and
// sweeps round the middle on a narrowing spiral, growing as it nears, a soft
// trail of light behind it that the mist gives way to. It lands in the
// middle in a ring of light, blinks, steps aside and the word "frontend" comes
// into focus beside it, its letters drawing together as they sharpen out of
// their own soft glow. Then the splash opens: the word goes out of focus,
// the night fades, the app (Main.qml, `reveal`) fades and grows into view
// and the Ghost flies onto the welcome Ghost, held hidden meanwhile, which
// then shows in its place and continues its motion: one Ghost throughout. A
// key or click skips to the Ghost in its place with the word, then opens.
// Not shown with reduced motion. Times are splash.js's, in ms from the
// scene's start: once three frames in a row come on time (24 ms apart), or
// 700 ms after the first frame at most. The mist is drawn at .6 of the
// window's pixels, as splash.js draws it with a GPU; without shaders (the
// software renderer) a soft light in the middle stands in for it.
Item {
    id: splash
    property var welcome: null // The WelcomeGhost it lands on, when shown.
    signal opening() // The app starts to fade in.
    signal finished()

    readonly property real size: 112 // --splash-size
    readonly property string word: "OpenGhost"
    readonly property bool shaded: GraphicsInfo.api !== GraphicsInfo.Software

    // splash.js: FLIGHT, LAND, TRAIL, AURA, START, WORD, HOLD, BLINK, OPEN, LEAVE.
    readonly property real flightAt: 220
    readonly property real flightDuration: 1650
    readonly property real landAt: flightAt + flightDuration
    readonly property real trailLife: 900
    readonly property real wordDelay: 40
    readonly property real wordStagger: 30
    readonly property real wordDuration: 560
    readonly property real wordTrack: 960
    readonly property real wordGap: 22
    readonly property real wordSize: 48
    readonly property real hold: 200
    readonly property real openDuration: 760
    readonly property real leaveWord: 200
    readonly property real leaveFly: 90
    readonly property real wordDone: wordDelay + word.length * wordStagger + wordDuration + hold

    // The scene clock: ms since the scene started (-1 before).
    property real now: -1
    property real t0: -1
    property real firstFrame: -1
    property real lastFrame: -1
    property int evenFrames: 0
    property real landedAt: -1  // Scene time of the landing.
    // Scene time the word and the step aside began: the frame after the
    // landing, as animations played in splash.js start on the next frame.
    property real wordAt: -1
    property real openAt: -1    // Scene time the opening began.
    property bool settled: false // The word and the step aside at their end.
    property bool skipped: false
    property bool cut: false    // Skipped in flight: no trail.
    property bool blinked: false
    // The flight's transform of the Ghost (.splash-fly): from the middle,
    // turned, scaled, and how far out of the mist.
    property real flyX: 0
    property real flyY: 0
    property real tilt: 0
    property real flyScale: 1
    property real presence: 0
    // The light's centre on the Ghost, its scale and presence (mist.frag).
    property vector4d glowAt: Qt.vector4d(width / 2, height / 2, 0.42, 0)
    property int trailCount: 0
    property vector4d trailBox: Qt.vector4d(-1, -1, -1, -1)
    property point landing
    property real landingScale: 1
    property bool lands: false

    // Progress the tests and the window read: the fly-in (0 … 1) and the
    // opening (0 … 1).
    readonly property real flight: now < 0 ? 0 : clamp((Math.min(now, landedAt < 0 ? now : landedAt) - flightAt) / flightDuration)
    readonly property real open: openAt < 0 ? 0 : clamp((now - openAt) / openDuration)
    readonly property real since: wordAt < 0 ? -1 : now - wordAt
    readonly property real openMs: openAt < 0 ? -1 : now - openAt
    // The app fading and growing into view (.app: opacity 0, scale .975 → 1).
    readonly property real reveal: Theme.motionAt(open)
    readonly property bool revealing: openAt >= 0

    anchors.fill: parent
    z: 1000

    function clamp(v) { return Math.min(1, Math.max(0, v)) }
    function smooth(t) { return t * t * (3 - 2 * t) }
    function easeOut(t) { return Theme.bezier(0, 0, 0.58, 1, clamp(t)) }

    // splash.js spot() and pose(): where the Ghost is at `u` of its way in,
    // and at scene time `t`, from the window's middle, with its speed.
    function spot(u) {
        const angle = (128 + 335 * u) * Math.PI / 180, r = 0.56 * width * Math.pow(1 - u, 1.3)
        return { x: Math.cos(angle) * r, y: Math.sin(angle) * r * height / width * 1.15 }
    }
    function pose(t) {
        const q = clamp((t - flightAt) / flightDuration), u = 1 - Math.pow(1 - q, 2.2)
        const here = spot(u), ahead = spot(Math.min(1, u + 0.01))
        const pace = 2.2 * Math.pow(1 - q, 1.2) / flightDuration / 0.01
        return { q: q, x: here.x, y: here.y, vx: (ahead.x - here.x) * pace, vy: (ahead.y - here.y) * pace,
                 scale: 0.42 + 0.58 * (1 - Math.pow(1 - u, 3)),
                 presence: smooth(clamp(q / 0.16)) }
    }

    // A frame: the scene starts once frames come evenly; the Ghost travels,
    // the mist follows it, the word and the opening keep their times.
    function frame(ms) {
        if (t0 < 0) {
            if (firstFrame < 0)
                firstFrame = ms
            evenFrames = lastFrame < 0 || ms - lastFrame < 24 ? evenFrames + 1 : 0
            lastFrame = ms
            if (evenFrames < 3 && ms - firstFrame < 700)
                return
            t0 = ms
        }
        const previous = now
        now = ms - t0
        if (landedAt < 0)
            travel(now, previous < 0 ? 0 : now - previous)
        else if (wordAt < 0)
            wordAt = now
        mist()
        if (landedAt >= 0 && !blinked && now - landedAt >= 200) {
            blinked = true
            ghost.blink()
        }
        if (wordAt >= 0 && openAt < 0 && (skipped || since >= wordDone))
            startOpening()
        if (openAt >= 0 && openMs >= (lands ? leaveFly + openDuration : openDuration))
            end()
    }
    function travel(t, dt) {
        const p = pose(t)
        if (p.q >= 1) {
            arrive(t)
            return
        }
        if (p.q <= 0)
            return
        const speed = Math.hypot(p.vx, p.vy) || 1
        // splash.js eases the lean a fifth of the way each frame; here per
        // 60 Hz frame's worth of time, whatever the display's rate.
        const k = 1 - Math.pow(0.8, dt / (1000 / 60))
        tilt += (Math.max(-1, Math.min(1, p.vx / 1.4)) * 16 - tilt) * k
        flyX = p.x
        flyY = p.y
        flyScale = p.scale
        presence = p.presence
        ghost.look(p.vx / speed * 4, p.vy / speed * 2.4, 200)
        glowAt = Qt.vector4d(width / 2 + p.x, height / 2 + p.y, p.scale, p.presence)
    }
    // Landed in the middle, where its own place is: the word comes in.
    function arrive(t) {
        landedAt = t
        flyX = 0
        flyY = 0
        tilt = 0
        flyScale = 1
        presence = 1
        ghost.look(4, 0.4, 2000)
    }
    // The trail behind it (splash.js draw()): how many points of the way,
    // and the box they and the trail's reach (140 px) fill.
    function mist() {
        if (landedAt >= 0) {
            const c = ghost.mapToItem(splash, ghost.width / 2, ghost.height / 2)
            glowAt = Qt.vector4d(c.x, c.y, 1, 1)
        }
        const end = Math.min(now, landAt)
        let count = 0, left = Infinity, top = Infinity, right = -Infinity, bottom = -Infinity
        for (let k = 0; k < 24 && !cut && now - end < trailLife; ++k) {
            const when = end - trailLife * Math.pow(k / 23, 1.6)
            if (when < flightAt)
                break
            const p = pose(when), x = width / 2 + p.x, y = height / 2 + p.y
            ++count
            left = Math.min(left, x); top = Math.min(top, y)
            right = Math.max(right, x); bottom = Math.max(bottom, y)
        }
        trailCount = count
        trailBox = count > 1 ? Qt.vector4d(left - 140, top - 140, right + 140, bottom + 140)
                             : Qt.vector4d(-1, -1, -1, -1)
    }

    FrameAnimation {
        running: splash.openAt < 0 || splash.openMs < 1000
        onTriggered: splash.frame(Theme.clock())
    }

    // .splash-bg: night falls over --chat-bg and the mist drifts through it;
    // it fades over 760 ms after 80 ms ('ease') as the splash opens.
    Item {
        id: stage
        objectName: "splashStage"
        anchors.fill: parent
        opacity: splash.openMs < 0 ? 1 : 1 - Theme.bezier(0.25, 0.1, 0.25, 1, splash.clamp((splash.openMs - 80) / 760))
        Rectangle {
            anchors.fill: parent
            color: Theme.chatBg
        }
        Loader {
            anchors.fill: parent
            active: splash.shaded
            sourceComponent: ShaderEffect {
                readonly property var tokens: Theme.splash
                function rgb(c) { return Qt.vector4d(c.r, c.g, c.b, 1) }
                property size view: Qt.size(width, height)
                property real time: Math.max(0, splash.now) / 1000
                property real show: splash.smooth(splash.clamp(splash.now / 700))
                property real pulse: splash.landedAt < 0 ? -1 : (splash.now - splash.landedAt) / 1000
                property real count: splash.trailCount
                property real life: splash.trailLife / 1000
                property real scene: Math.max(0, splash.now)
                property real trailEnd: Math.min(Math.max(0, splash.now), splash.landAt)
                property vector4d ghost: splash.glowAt
                property vector4d box: splash.trailBox
                property vector4d base: rgb(Theme.chatBg)
                property vector4d night: rgb(tokens.night)
                property vector4d deep: rgb(tokens.mistDeep)
                property vector4d lit: rgb(tokens.mistLit)
                property vector4d glow: rgb(tokens.glow)
                property vector4d core: rgb(tokens.core)
                property real mistAlpha: tokens.mistAlpha
                property real glowAlpha: tokens.glowAlpha
                property real trailGlow: tokens.trail
                property real shade: tokens.shade
                property real motes: tokens.motes
                fragmentShader: "qrc:/shaders/mist.frag.qsb"
                // The mist at .6 of the window's pixels (MIST.scale), stretched.
                layer.enabled: true
                layer.smooth: true
                layer.textureSize: Qt.size(Math.max(1, Math.round(width * Screen.devicePixelRatio * 0.6)),
                                           Math.max(1, Math.round(height * Screen.devicePixelRatio * 0.6)))
            }
        }
        // Without shaders: .splash-aura, a soft light in the middle that
        // rises over 900 ms and swells as the Ghost lands.
        Shape {
            id: aura
            visible: !splash.shaded
            readonly property real t: Math.max(0, splash.now)
            readonly property real risen: Theme.bezier(0.22, 1, 0.36, 1, splash.clamp(t / 900))
            readonly property real swell: splash.settled && splash.landedAt >= 0 ? 1 : splash.clamp((t - (splash.landAt - 120)) / 1400)
            readonly property real swellEased: Theme.bezier(0.42, 0, 0.58, 1, swell)
            x: (splash.width - 1200) / 2
            y: (splash.height - 1200) / 2
            width: 1200
            height: 1200
            opacity: swell <= 0 ? 0.62 * risen
                     : swellEased < 0.28 ? 0.62 + 0.38 * swellEased / 0.28 : 1 - 0.18 * (swellEased - 0.28) / 0.72
            scale: swell <= 0 ? 0.84 + 0.12 * risen
                   : swellEased < 0.28 ? 0.96 + 0.11 * swellEased / 0.28 : 1.07 - 0.07 * (swellEased - 0.28) / 0.72
            ShapePath {
                strokeColor: "transparent"
                fillGradient: RadialGradient {
                    centerX: 600; centerY: 600; centerRadius: 600
                    focalX: 600; focalY: 600
                    GradientStop { position: 0; color: Theme.splash.aura }
                    GradientStop { position: 1; color: "transparent" }
                }
                PathRectangle { width: 1200; height: 1200 }
            }
        }
    }
    // Swallow input while it plays; any key or click skips ahead.
    MouseArea {
        anchors.fill: parent
        enabled: splash.openAt < 0
        onPressed: splash.skip()
    }
    Keys.onPressed: event => splash.skip()
    focus: true

    readonly property real wordShift: (wordGap + wordText.contentWidth) / 2
    readonly property real shifted: settled ? 1 : since < 0 ? 0 : Theme.motionAt(clamp(since / 640))
    readonly property real flown: lands && openMs >= 0 ? Theme.motionAt(clamp((openMs - leaveFly) / openDuration)) : 0
    readonly property real faded: !lands && openMs >= 0 ? Theme.motionAt(clamp(openMs / openDuration)) : 0

    Ghost {
        id: ghost
        objectName: "splashGhost"
        width: splash.size
        height: splash.size * 70 / 64
        readonly property real baseX: (splash.width - width) / 2
        readonly property real baseY: (splash.height - height) / 2
        x: baseX + splash.flyX - splash.wordShift * splash.shifted
           + (splash.landing.x - baseX + splash.wordShift) * splash.flown
        y: baseY + splash.flyY + (splash.landing.y - baseY) * splash.flown
        transformOrigin: Item.Center
        rotation: splash.tilt
        scale: splash.flyScale * (1 + (splash.landingScale - 1) * splash.flown) * (1 - 0.2 * splash.faded)
        opacity: splash.presence * (1 - splash.faded)
        running: splash.visible
        color: Theme.accent
        eyeColor: Theme.chatBg
    }

    // The word, and over it the same word out of focus: each letter drawn
    // only as its own soft glow (text-shadow 0 0 10px), which the sharp
    // letters fade in over. Each letter starts 0.265 em farther out per
    // place and draws in over 960 ms; the sharp one sharpens over 560 ms
    // (opacity 0 until .18, ease-out), the soft one swells to 1 at .3 and
    // goes, each 30 ms after the one before, from 40 ms after the landing.
    readonly property real wordX: width / 2 - wordShift + size / 2 + wordGap
    readonly property real wordY: height / 2 - 0.04 * wordSize - wordText.height / 2
    readonly property real leaving: openMs < 0 ? 0 : easeOut(openMs / leaveWord)
    function letterAt(k) {
        return settled ? 1 : since < 0 ? 0 : easeOut((since - wordDelay - k * wordStagger) / wordDuration)
    }
    function trackAt(k) {
        return settled ? 0 : since < 0 ? k * 0.265 * wordSize
                                       : k * 0.265 * wordSize * (1 - Theme.bezier(0.16, 1, 0.3, 1, clamp(since / wordTrack)))
    }
    component Letter: Text {
        required property int index
        text: splash.word[index]
        color: Theme.accent
        font.pixelSize: splash.wordSize
        font.weight: Theme.weight(600)
        font.letterSpacing: -0.025 * splash.wordSize
        transform: Translate { x: splash.trackAt(index) }
    }
    Item {
        id: sharp
        x: splash.wordX
        y: splash.wordY
        width: wordText.width
        height: wordText.height
        visible: splash.since >= 0
        opacity: 1 - splash.leaving
        scale: 1 - 0.02 * splash.leaving
        // text-shadow: 0 0 34px rgba(var(--splash-glow-rgb), .2).
        TextGlow {
            source: sharpRow
            sigma: 17
            color: Theme.splash.glowRgb
            strength: 0.2
        }
        Row {
            id: sharpRow
            objectName: "splashWord"
            Repeater {
                model: splash.word.length
                delegate: Letter {
                    // Opacity 0, 0 at .18, then 1.
                    readonly property real p: splash.letterAt(index)
                    opacity: p < 0.18 ? 0 : (p - 0.18) / 0.82
                }
            }
        }
    }
    Item {
        id: haze
        x: splash.wordX
        y: splash.wordY
        width: wordText.width
        height: wordText.height
        visible: splash.since >= 0
        Row {
            id: hazeRow
            objectName: "splashHaze"
            Repeater {
                model: splash.word.length
                delegate: Letter {
                    // Opacity 0, 1 at .3, then 0; leaving, .35 at .3 of 260 ms.
                    readonly property real p: splash.letterAt(index)
                    readonly property real l: splash.openMs < 0 ? -1 : splash.easeOut(splash.openMs / (splash.leaveWord + 60))
                    opacity: l >= 0 ? (l < 0.3 ? 0.35 * l / 0.3 : 0.35 * (1 - l) / 0.7)
                                    : p < 0.3 ? p / 0.3 : (1 - p) / 0.7
                }
            }
        }
        TextGlow {
            source: hazeRow
            sigma: 5
            color: Theme.accent
            hideSource: true
        }
    }
    Text { // Measures the word for the step aside.
        id: wordText
        visible: false
        text: splash.word
        font.pixelSize: splash.wordSize
        font.weight: Theme.weight(600)
        font.letterSpacing: -0.025 * splash.wordSize
    }

    // Straight to the Ghost in its place, with the mist already in: the word
    // shows at once and the splash opens.
    function skip() {
        if (skipped || openAt >= 0)
            return
        skipped = true
        const ms = Theme.clock()
        if (t0 < 0)
            t0 = ms - 700
        now = ms - t0
        if (landedAt < 0) {
            cut = true // It never flew this way, so it leaves no trail.
            t0 = Math.min(t0, ms - 700)
            now = ms - t0
            arrive(now)
        }
        if (wordAt < 0)
            wordAt = now
        settled = true
        startOpening()
    }
    function startOpening() {
        if (openAt >= 0)
            return
        settled = true
        // Land where the welcome Ghost rests, when it is to be shown; else
        // fade away. It looks down for 1.16 s, as the welcome Ghost does.
        if (welcome && welcome.shown) {
            const there = welcome.mapToItem(splash, 0, 0)
            landing = Qt.point(there.x - (ghost.width - welcome.width) / 2,
                               there.y - (ghost.height - welcome.height) / 2)
            landingScale = welcome.width / ghost.width
            lands = true
            ghost.look(0, 1.4, openDuration + 400)
        }
        openAt = now
        splash.opening()
    }
    // The welcome Ghost takes over where this one landed.
    function end() {
        if (lands && welcome && welcome.shown) {
            ghost.visible = false
            welcome.arrive(ghost)
        }
        finished()
    }
}
