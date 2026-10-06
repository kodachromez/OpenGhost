import QtQuick
import OpenGhost.Cpp

// The run's working Ghost (chat.js's .message-status), after the newest row
// while OpenGhost works. It enters with ghost-in (450 ms); it collapses away while
// the answer streams (260 ms) and turns back where it is if work resumes
// before its exit ends; rows moving it glide it (320 ms). All on
// --motion-easing; with reduced motion it just appears and leaves, and reduced
// motion turned on mid-way ends each transition where it was going. One Ghost
// per run: its loop continues through the run's text and tool phases, and
// only a new run's Ghost starts afresh.
Item {
    id: slot
    property bool working: false
    // The window's run count: a new run's Ghost restarts its loop.
    property int run: 0
    property int started: -1 // The run this Ghost's loop belongs to.
    property real columnX: 0
    // The welcome Ghost is flying here: this one is hidden until it lands.
    property bool arriving: false
    readonly property alias ghost: ghost

    // Linear time of the exit (0 shown … 1 gone) and entrance (0 … 1); an
    // exit turned back retraces the same curve, as a reversed animation does.
    property real leaving: 1
    property real entering: 1
    readonly property real gone: Theme.motionAt(leaving)
    readonly property real shown: Theme.motionAt(entering)
    property real glide: 0
    property real lastY: -1

    implicitHeight: (33 + 14) * (1 - gone)
    height: implicitHeight
    visible: leaving < 1

    function show() {
        // A new run's Ghost starts afresh, unless it is still on screen
        // leaving: then it turns back and carries on.
        if (leaving >= 1 && started !== run)
            ghost.restart()
        started = run
        if (Theme.reducedMotion) {
            settle()
            return
        }
        if (leaving >= 1) { // Gone: it pops in again.
            glide = 0
            leaving = 0
            entering = 0
            entrance.restart()
            return
        }
        exit.to = 0
        exit.duration = 260 * leaving
        exit.restart()
    }
    function hide() {
        if (Theme.reducedMotion || leaving >= 1) {
            settle()
            return
        }
        exit.to = 1
        exit.duration = 260 * (1 - leaving)
        exit.restart()
    }
    // Every transition at its end: shown while working, else gone.
    function settle() {
        exit.stop()
        entrance.stop()
        glideBack.stop()
        glide = 0
        entering = 1
        leaving = working ? 0 : 1
    }
    Connections {
        target: Theme
        function onReducedMotionChanged() {
            if (Theme.reducedMotion)
                slot.settle()
        }
    }
    onWorkingChanged: working ? show() : hide()
    Component.onCompleted: if (working) show()

    // Rows above changed its place: it starts from where it was shown.
    onYChanged: {
        if (visible && lastY >= 0 && !Theme.reducedMotion && Math.abs(lastY - y) >= 1) {
            glide += lastY - y
            glideBack.restart()
        }
        lastY = y
    }

    NumberAnimation {
        id: exit
        target: slot
        property: "leaving"
    }
    NumberAnimation {
        id: entrance
        target: slot
        property: "entering"
        to: 1
        duration: 450
    }
    NumberAnimation {
        id: glideBack
        target: slot
        property: "glide"
        to: 0
        duration: 320
        easing.type: Easing.Bezier
        easing.bezierCurve: Theme.motion
    }

    Ghost {
        id: ghost
        objectName: "workingGhost"
        x: slot.columnX
        y: 14 * (1 - slot.gone) + 6 * (1 - slot.shown) + slot.glide
        width: 30
        height: 33
        transformOrigin: Item.TopLeft
        scale: (1 - 0.3 * slot.gone) * (0.7 + 0.3 * slot.shown)
        opacity: slot.arriving ? 0 : (1 - slot.gone) * slot.shown
        running: slot.visible
        reducedMotion: Theme.reducedMotion
        color: Theme.accent
        eyeColor: Theme.chatBg
    }
}
