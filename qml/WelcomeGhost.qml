import QtQuick
import QtQuick.Effects
import OpenGhost.Native

// The welcome Ghost (welcome-ghost.js) over an empty conversation. It rises
// in, watches the pointer and glances down at typing. When the first message
// leaves, it flies to the working Ghost and becomes it (720 ms; the
// working Ghost stays hidden until it lands). An answer that dismisses the
// working Ghost mid-flight makes it leave where it is (260 ms), and work
// resuming turns it back to fly on. With no working Ghost it just leaves.
// While `held` (the splash plays) it waits hidden; a splash landing on it
// shows it at rest, continuing the splash's Ghost. With reduced motion none
// of this moves, and reduced motion turned on mid-way ends each transition
// where it was going.
Item {
    id: welcome
    property bool shown: false
    property bool held: false
    readonly property bool showing: shown && !held
    property var slot: null // The WorkingGhost it hands over to.
    readonly property alias ghost: ghost
    readonly property bool flying: phase === "flying"
    property string phase: "hidden" // hidden, shown, leaving, flying

    width: 96
    height: 105

    // welcome-ghost.js EYES.
    readonly property real eyeX: 4
    readonly property real eyeUp: 2.4
    readonly property real eyeDown: 2.2
    readonly property real eyeCenter: 0.49
    readonly property real eyeReach: 90

    // Entrance (.welcome.is-shown) and exit (.welcome.is-leaving), linear.
    property real appear: 0
    property real away: 0
    // The flight's linear time, and the flying Ghost's own exit.
    property real flight: 0
    property real flee: 0
    property point origin
    property point destination
    property real toScale: 1

    readonly property real leave: Theme.motionAt(away)

    visible: phase !== "hidden"
    onShowingChanged: showing ? enter() : depart()
    Component.onCompleted: if (showing) enter()

    function enter() {
        if (phase === "shown") // A splash landed it already.
            return
        stopAll()
        phase = "shown"
        flight = 0
        flee = 0
        away = 0
        ghost.restart()
        if (Theme.reducedMotion) {
            appear = 1
            return
        }
        appear = 0
        rising.restart()
    }
    function depart() {
        if (phase === "hidden")
            return
        if (Theme.reducedMotion) {
            reset()
            return
        }
        const target = slot && slot.working ? slot.ghost : null
        if (!target) {
            phase = "leaving"
            ghost.look(0, -eyeUp, 520)
            leavingAnim.restart()
            return
        }
        // Fly to where the working Ghost is shown, and become it.
        rising.complete()
        const here = ghost.mapToItem(welcome.parent, 0, 0)
        const there = target.mapToItem(welcome.parent, 0, 0)
        origin = Qt.point(here.x, here.y)
        destination = Qt.point(there.x, there.y)
        toScale = target.width / ghost.width
        const dx = destination.x + target.width / 2 - (origin.x + ghost.width / 2)
        const dy = destination.y + target.height / 2 - (origin.y + ghost.height / 2)
        const length = Math.hypot(dx, dy) || 1
        ghost.look(dx / length * eyeX, dy / length * eyeUp, 720)
        tilt = Math.sign(dx) * 8
        phase = "flying"
        slot.arriving = true
        flight = 0
        flee = 0
        flightAnim.restart()
    }
    property real tilt: 8
    function reset() {
        stopAll()
        if (slot)
            slot.arriving = false
        phase = "hidden"
    }
    function stopAll() {
        rising.stop()
        leavingAnim.stop()
        flightAnim.stop()
        fleeAnim.stop()
    }
    // Landed on the working Ghost: it takes over this one's motion.
    function land() {
        if (slot && slot.working)
            slot.ghost.continueFrom(ghost)
        reset()
    }
    // The splash's Ghost landed here: shown at rest, with its motion.
    function arrive(other) {
        stopAll()
        ghost.continueFrom(other)
        flight = 0
        flee = 0
        away = 0
        appear = 1
        phase = "shown"
    }
    // Every transition at its end: a flight lands, an exit is gone.
    function settle() {
        if (phase === "flying")
            land()
        else if (phase === "leaving")
            reset()
        rising.stop()
        appear = 1
    }
    Connections {
        target: Theme
        function onReducedMotionChanged() {
            if (Theme.reducedMotion)
                welcome.settle()
        }
    }

    // A first answer while flying: this Ghost leaves where it is; work
    // resuming turns it back and it flies on.
    Connections {
        target: welcome.slot
        enabled: welcome.flying
        function onWorkingChanged() {
            if (!welcome.slot.working) {
                flightAnim.pause()
                fleeAnim.to = 1
                fleeAnim.duration = 260 * (1 - welcome.flee)
                fleeAnim.restart()
            } else {
                fleeAnim.to = 0
                fleeAnim.duration = 260 * welcome.flee
                fleeAnim.restart()
                flightAnim.resume()
            }
        }
    }

    // The pointer, in the window's coordinates, as the Ghost's gaze.
    function lookAt(point) {
        if (phase !== "shown")
            return
        const box = welcome.mapFromItem(null, point.x, point.y)
        const dx = box.x - width / 2, dy = box.y - height * eyeCenter
        const length = Math.hypot(dx, dy) || 1, reach = length / (length + eyeReach)
        ghost.look(dx / length * reach * eyeX, dy / length * reach * (dy < 0 ? eyeUp : eyeDown))
    }
    function typing() {
        if (phase === "shown")
            ghost.look(0, eyeDown, 1400)
    }

    NumberAnimation {
        id: rising
        target: welcome
        property: "appear"
        from: 0
        to: 1
        duration: 1050 // 150 ms delay, then 900 ms (opacity takes 450 ms).
    }
    NumberAnimation {
        id: leavingAnim
        target: welcome
        property: "away"
        from: 0
        to: 1
        duration: 520
        onFinished: welcome.reset()
    }
    NumberAnimation {
        id: flightAnim
        target: welcome
        property: "flight"
        from: 0
        to: 1
        duration: 720
        onFinished: welcome.land()
    }
    NumberAnimation {
        id: fleeAnim
        target: welcome
        property: "flee"
        onFinished: if (welcome.flee >= 1) welcome.reset()
    }

    // Timing of the entrance: a 150 ms delay, opacity over 450 ms, the rise
    // over 900 ms.
    readonly property real riseT: Math.max(0, Math.min(1, (appear * 1050 - 150) / 900))
    readonly property real fadeT: Math.max(0, Math.min(1, (appear * 1050 - 150) / 450))
    readonly property real flyX: Theme.bezier(0.5, 0, 0.3, 1, flight)
    readonly property real flyY: Theme.bezier(0.25, 0.7, 0.3, 1, flight)
    readonly property real flyScale: Theme.bezier(0.45, 0, 0.3, 1, flight)
    // rotate: 0 → tilt at 40% → 0, ease-in-out over the whole flight.
    readonly property real flyTilt: {
        const t = Theme.bezier(0.42, 0, 0.58, 1, flight)
        return t < 0.4 ? tilt * t / 0.4 : tilt * (1 - (t - 0.4) / 0.6)
    }
    readonly property real fleeEased: Theme.motionAt(flee)
    // filter: blur(8px) clears as it comes in (0.5 s ease after 150 ms); it
    // blurs to 6 px as it leaves (0.45 s) while the leaving mask wipes it
    // away upward from its foot (a 40 % ramp from −40 % to 100 %, 0.5 s
    // ease-out).
    readonly property real blurT: Math.max(0, Math.min(1, (appear * 1050 - 150) / 500))
    readonly property real blurPx: flying ? 0
        : phase === "leaving" ? 6 * Theme.bezier(0.25, 0.1, 0.25, 1, Math.min(1, away * 520 / 450))
        : 8 * (1 - Theme.bezier(0.25, 0.1, 0.25, 1, blurT))
    readonly property real wipe: phase === "leaving"
        ? -0.4 + 1.4 * Theme.bezier(0, 0, 0.58, 1, Math.min(1, away * 520 / 500)) : -0.4
    // What the mask takes away (1 − mask), a fraction `from` of the height
    // up from the foot.
    function wiped(from) {
        return 1 - Math.max(0, Math.min(1, (from - wipe) / 0.4))
    }

    Ghost {
        id: ghost
        objectName: "welcomeGhost"
        width: welcome.width
        height: welcome.height
        running: welcome.visible
        reducedMotion: Theme.reducedMotion
        color: Theme.accent
        eyeColor: Theme.chatBg
        transformOrigin: Item.Center
        readonly property real flyScaleValue: 1 + (welcome.toScale - 1) * welcome.flyScale
        x: welcome.flying ? (welcome.destination.x - welcome.origin.x) * welcome.flyX
                            + (welcome.toScale - 1) * welcome.width / 2 * welcome.flyScale : 0
        y: welcome.flying ? (welcome.destination.y - welcome.origin.y) * welcome.flyY
                            + (welcome.toScale - 1) * welcome.height / 2 * welcome.flyScale
         : welcome.phase === "leaving" ? -26 * welcome.leave
         : 14 * (1 - Theme.bezier(0.34, 1.45, 0.64, 1, welcome.riseT))
        scale: welcome.flying ? flyScaleValue * (1 - 0.3 * welcome.fleeEased)
             : welcome.phase === "leaving" ? 1 - 0.1 * welcome.leave
             : 0.6 + 0.4 * Theme.bezier(0.34, 1.45, 0.64, 1, welcome.riseT)
        rotation: welcome.flying ? welcome.flyTilt : 0
        opacity: welcome.flying ? 1 - welcome.fleeEased
               : welcome.phase === "leaving" ? 1 - Theme.bezier(0.25, 0.1, 0.25, 1, Math.min(1, welcome.away * 520 / 450))
               : Theme.bezier(0.25, 0.1, 0.25, 1, welcome.fadeT)
        // MultiEffect's blur is about σ = .27 · blurMax · blur.
        layer.enabled: !Theme.reducedMotion && (welcome.blurPx > 0.05 || welcome.wipe > -0.4)
        layer.effect: MultiEffect {
            blurEnabled: true
            blurMax: 32
            blur: welcome.blurPx / 8.64
        }
        // The mask, drawn as the empty conversation's background over the
        // Ghost inside its layer, which the layer's opacity then fades as
        // CSS fades a masked element.
        Rectangle {
            id: wipeMask
            readonly property real near: Math.max(0, Math.min(1, 1 - welcome.wipe - 0.4))
            readonly property real far: Math.max(0, Math.min(1, 1 - welcome.wipe))
            anchors.fill: parent
            visible: welcome.wipe > -0.4
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.alpha(Theme.chatBg, welcome.wiped(1)) }
                GradientStop { position: wipeMask.near; color: Theme.alpha(Theme.chatBg, welcome.wiped(1 - wipeMask.near)) }
                GradientStop { position: wipeMask.far; color: Theme.alpha(Theme.chatBg, welcome.wiped(1 - wipeMask.far)) }
                GradientStop { position: 1; color: Theme.alpha(Theme.chatBg, welcome.wiped(0)) }
            }
        }
    }
}
