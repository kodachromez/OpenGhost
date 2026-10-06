import QtQuick
import QtQuick.Controls
import OpenGhost.Cpp

// OpenGhost's scroll bar (scrollbar.js, .scrollbar): a 12 px strip over the
// view's edge holding a 4 px thumb, 2 px from its ends and at least 24 px
// long, in the tertiary grey. Over the strip, or dragged, the thumb is 6 px
// wide in the secondary grey. It shows while there is anything to scroll.
// Its placement is the owner's (.thread-scrollbar, .chats-scrollbar,
// .composer-scrollbar).
// The drawn thumb's length springs to a new size (260/32). A wheel turn
// that meets an end of `view`, or brings it there within 400 ms, squishes
// the thumb against that end: up to a quarter of its length, by the turn's
// force, over 280 ms. Neither with reduced motion.
ScrollBar {
    id: bar
    property Flickable view
    readonly property bool scrollable: size > 0 && size < 1
    readonly property bool lit: hovered || pressed

    width: 12
    padding: 0
    topPadding: 2
    bottomPadding: 2
    minimumSize: availableHeight > 0 ? Math.min(1, 24 / availableHeight) : 0
    hoverEnabled: true
    opacity: scrollable ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
    background: Item {}
    // Where Qt places the thumb, for pressing and dragging; the drawn one
    // below takes its place with the springs.
    contentItem: Item { implicitWidth: 12 }

    readonly property real goal: visualSize * availableHeight
    readonly property real ratio: visualSize < 1 ? visualPosition / (1 - visualSize) : 0
    property real length: goal
    property real speed: 0
    onScrollableChanged: if (scrollable) snapLength()
    onGoalChanged: if (Theme.reducedMotion || !scrollable) snapLength()
    function snapLength() {
        length = goal
        speed = 0
    }

    // scrollbar.js edges: the last wheel turn toward each, its force, and
    // the squish playing there.
    readonly property var edges: ({
        top: { wheel: -Infinity, force: 0, start: -Infinity, amp: 0 },
        bottom: { wheel: -Infinity, force: 0, start: -Infinity, amp: 0 }
    })
    property real now: 0
    function squishAt(edge, now) {
        const u = (now - edge.start) / 280
        return u > 0 && u < 1 ? edge.amp * Math.pow(Math.sin(Math.PI * u), 2) : 0
    }
    function bump(edge, now, fresh) {
        if (Theme.reducedMotion)
            return
        const q = squishAt(edge, now), amp = 0.25 * (1 - Math.exp(-edge.force / 40))
        const rising = now - edge.start < 140
        if (!fresh && !(rising && amp > edge.amp))
            return
        edge.amp = Math.max(q, amp)
        if (!edge.amp)
            return
        edge.start = now - 280 * Math.asin(Math.sqrt(q / edge.amp)) / Math.PI
        bar.now = now
        bar.squishing = true
    }
    function limit() { return view ? view.contentHeight - view.height : 0 }
    function scrolled() { return view ? view.contentY - view.originY : 0 }
    // A view that takes its own wheel turns (WheelScroll) reports them here.
    property bool watchesWheel: true
    function wheeled(deltaY) {
        const max = bar.limit()
        if (max <= 1)
            return
        const down = deltaY > 0, edge = down ? bar.edges.bottom : bar.edges.top
        const now = Date.now(), fresh = now - edge.wheel > 200
        edge.wheel = now
        edge.force = Math.max(fresh ? 0 : edge.force, Math.abs(deltaY))
        if (down ? bar.scrolled() < max - 1 : bar.scrolled() > 0)
            return
        bar.bump(edge, now, fresh)
    }
    WheelWatch {
        target: bar.watchesWheel ? bar.view : null
        onWheeled: deltaY => bar.wheeled(deltaY)
    }
    // Arriving at an end within 400 ms of a wheel turn toward it.
    property real last: 0
    Connections {
        target: bar.view
        function onContentYChanged() {
            const max = bar.limit(), before = bar.last, at = bar.scrolled(), now = Date.now()
            bar.last = at
            if (max > 1 && at >= max - 1 && before < max - 1 && now - bar.edges.bottom.wheel < 400)
                bar.bump(bar.edges.bottom, now, true)
            else if (max > 1 && at <= 0 && before > 0 && now - bar.edges.top.wheel < 400)
                bar.bump(bar.edges.top, now, true)
        }
    }
    // Ticks while the thumb is off its length or a squish plays.
    property bool squishing: false
    FrameAnimation {
        id: clock
        running: bar.scrollable && (bar.squishing || Math.abs(bar.goal - bar.length) > 0.05
                                    || Math.abs(bar.speed) > 0.05)
        onTriggered: {
            const dt = Math.min(frameTime, 0.032)
            if (Theme.reducedMotion)
                bar.snapLength()
            const steps = Math.max(1, Math.ceil(dt / 0.008)), h = dt / steps
            let x = bar.length, v = bar.speed
            for (let i = 0; i < steps; ++i) {
                v += ((bar.goal - x) * 260 - v * 32) * h
                x += v * h
            }
            if (Math.abs(bar.goal - x) > 0.05 || Math.abs(v) > 0.05) {
                bar.length = x
                bar.speed = v
            } else {
                bar.snapLength()
            }
            bar.now = Date.now()
            bar.squishing = bar.now - bar.edges.top.start < 280 || bar.now - bar.edges.bottom.start < 280
        }
    }

    Rectangle {
        objectName: "scrollThumb"
        readonly property real cutTop: bar.length * Math.min(0.25, bar.squishAt(bar.edges.bottom, bar.now))
        readonly property real cutBottom: bar.length * Math.min(0.25, bar.squishAt(bar.edges.top, bar.now))
        x: (bar.width - width) / 2
        y: bar.topPadding + (bar.availableHeight - bar.length) * bar.ratio + cutTop
        width: bar.lit ? 6 : 4
        height: Math.max(0, bar.length - cutTop - cutBottom)
        radius: 3
        color: bar.lit ? Theme.secondary : Theme.tertiary
        // CSS ease.
        Behavior on width {
            NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
        }
        Behavior on color {
            ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
        }
    }
}
