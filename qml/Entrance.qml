import QtQuick
import OpenGhost.Native

// One of OpenGhost's entry keyframes (styles.css md-rise, md-pop, md-bar,
// md-row, md-chip, …) as a progress: play() runs `value` from 0 to 1 over
// `duration` ms after `delay` ms, on `curve` (the motion easing unless
// set), holding 0 through the delay as CSS's `both` fill does. It rests at
// 1, and finishes at once when reduced motion turns on.
QtObject {
    id: entrance
    property real value: 1
    property int duration: 450
    property int delay: 0
    property var curve: Theme.motion
    readonly property bool running: run.running
    function play() {
        if (Theme.reducedMotion)
            return
        value = 0
        run.restart()
    }
    property SequentialAnimation run: SequentialAnimation {
        PauseAnimation { duration: Math.max(0, entrance.delay) }
        NumberAnimation {
            target: entrance
            property: "value"
            from: 0
            to: 1
            duration: entrance.duration
            easing.type: Easing.Bezier
            easing.bezierCurve: entrance.curve
        }
    }
    property Connections reduced: Connections {
        target: Theme
        enabled: run.running
        function onReducedMotionChanged() { run.complete() }
    }
}
