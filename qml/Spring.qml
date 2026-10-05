import QtQuick
import OpenGhost.Native

// A damped spring integrated as OpenGhost's are (chat-list.js,
// search-field.js): acceleration (goal − value)·k − velocity·c, explicit
// Euler in steps of at most `step` s, frames capped at 32 ms. It ticks only
// while moving; with reduced motion it jumps to the goal.
QtObject {
    id: spring
    property real goal
    property real value
    property real velocity
    property real k: 300
    property real c: 28
    property real step: 0.008
    // At rest once this close to the goal and this slow.
    property real within: 0.0005
    property real slower: 0.002
    readonly property bool moving: clock.running

    function snap() {
        clock.stop()
        value = goal
        velocity = 0
    }
    onGoalChanged: {
        if (Theme.reducedMotion)
            snap()
        else if (!clock.running)
            clock.start()
    }
    property FrameAnimation clock: FrameAnimation {
        onTriggered: {
            const dt = Math.max(0, Math.min(frameTime, 0.032))
            const steps = Math.max(1, Math.ceil(dt / spring.step)), h = dt / steps
            let x = spring.value, v = spring.velocity
            for (let i = 0; i < steps; ++i) {
                v += ((spring.goal - x) * spring.k - v * spring.c) * h
                x += v * h
            }
            if (Math.abs(spring.goal - x) < spring.within && Math.abs(v) < spring.slower)
                spring.snap()
            else {
                spring.value = x
                spring.velocity = v
            }
        }
    }
}
