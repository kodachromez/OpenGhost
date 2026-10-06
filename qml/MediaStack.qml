import QtQuick
import QtQuick.Effects
import OpenGhost.Cpp

// A stack of pictures to leaf through (media-slider.js and styles.css
// .media): the one on top whole, the next ones fanned out behind it, more
// when hovered. A drag, a sideways swipe, the arrows, the dots or the arrow
// keys turn it; the stack follows the pointer, then springs to the picture
// it settles on. A click that was not a drag is `opened` (the gallery opens
// the page the picture is from). Pictures are MediaLoader's, already loaded:
// `images` are {url, source, width, height, caption}.
FocusScope {
    id: stack
    objectName: "mediaStack"
    readonly property bool selectionControl: true
    required property var images
    property real available: 380
    property int index: 0
    signal opened(int index)

    readonly property int count: images.length
    readonly property bool many: count > 1
    readonly property real raw: count > 0 && images[0].width > 0 && images[0].height > 0 ? images[0].width / images[0].height : 4 / 3
    readonly property real ratio: MediaLoader.stackRatio(raw, many)
    width: Math.min(available, MediaLoader.stackWidth(ratio))
    height: width / ratio
    activeFocusOnTab: many
    Accessible.role: Accessible.Grouping
    Accessible.name: "Photos and videos"

    // media-slider.js FAN, LEAVE, SPRING, SPREAD, DRAG_SLOP, FLING, RUBBER, WHEEL, DOTS.
    readonly property var fan: ({
            turn: 5,
            depth: 3,
            scale: 0.035,
            lift: 3,
            shade: 0.16,
            spread: 0.4
        })
    readonly property var leave: ({
            shift: 0.9,
            turn: 9,
            scale: 0.04,
            fade0: 0.5,
            fade1: 1,
            reach: 1.5
        })
    readonly property real clickSlop: 5

    property real pos: 0
    property real vel: 0
    property real spread: 0
    property real spreadVel: 0
    property bool hover: false
    property var drag: null
    property var swipe: null
    // Whether the stack follows the pointer or a swipe (not its spring).
    property bool dragMoved: false
    property bool swiping: false

    function clamp(v, lo, hi) {
        return Math.min(hi, Math.max(lo, v));
    }
    function smoothstep(a, b, v) {
        const t = clamp((v - a) / (b - a), 0, 1);
        return t * t * (3 - 2 * t);
    }
    function rubber(d) {
        return (1 - 1 / (d * 0.55 / 0.18 + 1)) * 0.18;
    }
    function shape(raw) {
        const last = count - 1;
        if (raw < 0)
            return -rubber(-raw);
        if (raw > last)
            return last + rubber(raw - last);
        return raw;
    }
    function go(i) {
        index = clamp(i, 0, count - 1);
        wake();
    }
    function wake() {
        if (Theme.reducedMotion) {
            pos = index;
            vel = 0;
            spread = 0;
            spreadVel = 0;
            return;
        }
        clock.running = true;
    }
    readonly property bool following: dragMoved || swiping
    function tick(dt) {
        dt = Math.min(Math.max(dt, 0), 0.032);
        const goal = hover && !dragMoved ? 1 : 0;
        if (Theme.reducedMotion) {
            wake();
            clock.running = false;
            return;
        }
        const steps = Math.max(1, Math.ceil(dt / 0.004)), h = dt / steps, follow = following;
        let p = pos, v = vel, s = spread, sv = spreadVel;
        for (let n = 0; n < steps; n++) {
            if (!follow) {
                v += ((index - p) * 210 - v * 29) * h;
                p += v * h;
            }
            sv += ((goal - s) * 240 - sv * 24) * h;
            s += sv * h;
        }
        if (!follow && Math.abs(index - p) < 0.0005 && Math.abs(v) < 0.002) {
            p = index;
            v = 0;
        }
        const spreading = Math.abs(goal - s) > 0.001 || Math.abs(sv) > 0.005;
        if (!spreading) {
            s = goal;
            sv = 0;
        }
        pos = p;
        vel = v;
        spread = s;
        spreadVel = sv;
        if (!spreading && (follow || pos === index))
            clock.running = false;
    }
    FrameAnimation {
        id: clock
        onTriggered: stack.tick(frameTime)
    }
    onHoverChanged: wake()
    // Other pictures make a new stack: it starts again at the first.
    onImagesChanged: {
        index = 0;
        pos = 0;
        vel = 0;
    }

    // Where picture k stands: behind (fanned) or gone past (leaving left).
    function place(k) {
        const d = k - pos, w = width;
        if (d >= 0) {
            const t = Math.min(d, fan.depth), open = 1 + fan.spread * spread;
            return {
                x: 0,
                y: -t * fan.lift * open,
                turn: t * fan.turn * open,
                scale: 1 - t * fan.scale,
                shade: Math.min(t, 2) * fan.shade,
                opacity: 1 - smoothstep(fan.depth - 1, fan.depth, d),
                z: 200 - k
            };
        }
        const u = Math.min(-d, leave.reach);
        return {
            x: -u * w * leave.shift,
            y: 0,
            turn: -u * leave.turn,
            scale: 1 - u * leave.scale,
            shade: 0,
            opacity: 1 - smoothstep(leave.fade0, leave.fade1, u),
            z: 300 + k
        };
    }

    readonly property color mediaBg: Theme.light ? Qt.rgba(238 / 255, 238 / 255, 238 / 255, 1) : Qt.rgba(20 / 255, 20 / 255, 20 / 255, 1)

    Item {
        id: picturePlane
        anchors.fill: parent
        Repeater {
            model: stack.images
            delegate: Item {
                id: card
                objectName: "mediaCard"
                required property var modelData
                required property int index
                readonly property var p: stack.place(index)
                width: stack.width
                height: stack.height
                visible: p.opacity > 0.001
                opacity: p.opacity
                z: p.z
                transform: [
                    Scale {
                        origin.x: card.width / 2
                        origin.y: card.height * 1.2
                        xScale: card.p.scale
                        yScale: card.p.scale
                    },
                    Rotation {
                        origin.x: card.width / 2
                        origin.y: card.height * 1.2
                        angle: card.p.turn
                    },
                    Translate {
                        x: card.p.x
                        y: card.p.y
                    }
                ]
                Accessible.role: Accessible.Graphic
                Accessible.name: modelData.caption || ("Photo " + (index + 1) + " of " + stack.count)
                Accessible.ignored: index !== stack.index

                BoxShadow { // 0 10px 28px rgba(0, 0, 0, .32 × --shadow)
                    anchors.fill: parent
                    radius: 18
                    blur: 28
                    offsetY: 10
                    color: Qt.rgba(0, 0, 0, 0.32 * Theme.shadow)
                }
                Item {
                    anchors.fill: parent
                    layer.enabled: true
                    layer.effect: MultiEffect {
                        maskEnabled: true
                        maskThresholdMin: 0.5
                        maskSpreadAtMin: 0.5
                        maskSource: cardMask
                    }
                    Rectangle {
                        anchors.fill: parent
                        color: stack.mediaBg
                    }
                    // A picture of another shape than the frame has itself,
                    // blurred and dimmed, filling around it.
                    Loader {
                        anchors.fill: parent
                        active: MediaLoader.needsBackdrop(card.modelData.width, card.modelData.height, stack.ratio)
                        sourceComponent: Item {
                            clip: true
                            Image {
                                id: back
                                x: -parent.width * 0.12
                                y: -parent.height * 0.12
                                width: parent.width * 1.24
                                height: parent.height * 1.24
                                source: card.modelData.source
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: false
                                cache: false
                                visible: false
                            }
                            // blur(24px) saturate(1.3) brightness(0.6): σ ≈ .27 · blurMax · blur.
                            MultiEffect {
                                source: back
                                anchors.fill: back
                                blurEnabled: true
                                blurMax: 96
                                blur: 24 / (0.27 * 96)
                                saturation: 0.3
                            }
                            Rectangle { // brightness(0.6) multiplies: 40 % black over it.
                                anchors.fill: parent
                                color: Qt.rgba(0, 0, 0, 0.4)
                            }
                        }
                    }
                    Image {
                        objectName: "mediaImage"
                        anchors.fill: parent
                        source: card.modelData.source
                        fillMode: Image.PreserveAspectFit
                        asynchronous: false
                        cache: false
                        smooth: true
                        mipmap: true
                    }
                    Rectangle { // .media-shade
                        anchors.fill: parent
                        color: "black"
                        opacity: card.p.shade
                    }
                }
                Rectangle { // .media-card::after
                    anchors.fill: parent
                    radius: 18
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.alpha(Theme.fg, 0.07)
                }
            }
        }
    } // picturePlane: no overlay may be captured into its own backdrop.
    Rectangle {
        id: cardMask
        width: stack.width
        height: stack.height
        radius: 18
        visible: false
        layer.enabled: true
    }

    // The pointer: a drag turns the stack, a click opens the picture on top.
    MouseArea {
        id: pointer
        objectName: "mediaPointer"
        anchors.fill: parent
        z: 400
        hoverEnabled: true
        preventStealing: true
        cursorShape: stack.dragMoved ? Qt.ClosedHandCursor : stack.many ? Qt.OpenHandCursor : Qt.PointingHandCursor
        property point from: Qt.point(0, 0)
        onEntered: stack.hover = true
        onExited: stack.hover = false
        onPressed: mouse => {
            from = Qt.point(mouse.x, mouse.y);
            stack.forceActiveFocus();
            if (stack.many)
                stack.drag = {
                    x: mouse.x,
                    from: stack.pos,
                    start: stack.index,
                    moved: false,
                    samples: [[Date.now(), mouse.x]]
                };
        }
        onPositionChanged: mouse => {
            const drag = stack.drag;
            if (!drag || !pressed)
                return;
            const dx = mouse.x - drag.x;
            if (!drag.moved) {
                if (Math.abs(dx) < 4)
                    return;
                drag.moved = true;
                stack.dragMoved = true;
                stack.wake();
            }
            const now = Date.now();
            drag.samples.push([now, mouse.x]);
            while (drag.samples.length > 2 && now - drag.samples[0][0] > 100)
                drag.samples.shift();
            stack.pos = stack.shape(drag.from - dx / stack.width);
            stack.vel = 0;
        }
        onReleased: mouse => {
            const drag = stack.drag;
            stack.drag = null;
            stack.dragMoved = false;
            if (drag && drag.moved) {
                const a = drag.samples[0], b = drag.samples[drag.samples.length - 1];
                stack.vel = b[0] > a[0] ? -(b[1] - a[1]) / (b[0] - a[0]) * 1000 / stack.width : 0;
                stack.go(stack.clamp(Math.round(stack.pos + stack.vel * 0.22), drag.start - 1, drag.start + 1));
                return;
            }
            if (mouse.button === Qt.LeftButton && Math.hypot(mouse.x - from.x, mouse.y - from.y) <= stack.clickSlop)
                stack.opened(stack.index);
        }
        onCanceled: {
            stack.drag = null;
            stack.dragMoved = false;
            stack.wake();
        }
        // Qt deltas have the opposite sign to DOM wheel deltas. Prefer
        // trackpad pixels on BOTH axes (angleDelta may be zero).
        onWheel: wheel => {
            const pixels = wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0;
            const dx = pixels ? -wheel.pixelDelta.x : -wheel.angleDelta.x / 120 * 48;
            const dy = pixels ? -wheel.pixelDelta.y : -wheel.angleDelta.y / 120 * 48;
            wheel.accepted = stack.wheelStep(dx, dy, Date.now());
        }
    }
    // media-slider.js onWheel/commitSwipe, including inertia-tail suppression
    // and a fresh growing/reversed gesture after fading. Kept callable for tests.
    function wheelStep(dx, dy, now) {
        if (!many || Math.abs(dx) <= Math.abs(dy) || !width || drag)
            return false;
        const size = Math.abs(dx), dir = Math.sign(dx) || 1;
        let gesture = swipe;
        if (gesture) {
            const settled = gesture.done ? now - gesture.doneAt > 140 : gesture.fading;
            const fresh = settled && (dir !== gesture.dir || size > gesture.low * 1.8 + 1.5);
            if (fresh) {
                if (!gesture.done)
                    commitSwipe();
                gesture = null;
            } else if (gesture.done) {
                gesture.low = Math.min(gesture.low, size);
                swipeEnd.restart();
                return true;
            }
        }
        if (gesture)
            gesture.speed = gesture.speed * 0.6 + dx / width / (Math.max(4, now - gesture.at) / 1000) * 0.4;
        else
            gesture = {
                base: index,
                from: pos,
                sum: 0,
                dir: dir,
                peak: 0,
                low: Infinity,
                fading: false,
                at: now,
                speed: 0,
                done: false,
                doneAt: 0
            };
        gesture.sum += dx;
        gesture.dir = dir;
        gesture.peak = Math.max(gesture.peak, size);
        if (size < gesture.peak * 0.6)
            gesture.fading = true;
        if (gesture.fading)
            gesture.low = Math.min(gesture.low, size);
        gesture.at = now;
        swipe = gesture;
        swiping = true;
        pos = shape(gesture.from + gesture.sum / width);
        vel = 0;
        const moved = Math.abs(pos - gesture.base);
        if (moved >= 0.3 || (gesture.fading && moved >= 0.12))
            commitSwipe();
        swipeEnd.restart();
        return true;
    }
    function commitSwipe() {
        const gesture = swipe, moved = pos - gesture.base;
        gesture.done = true;
        gesture.doneAt = gesture.at;
        gesture.low = Infinity;
        swiping = false;
        vel = clamp(gesture.speed, -8, 8);
        go(gesture.base + (Math.abs(moved) >= 0.12 ? Math.sign(moved) : 0));
    }
    Timer {
        id: swipeEnd
        interval: 100
        onTriggered: {
            if (stack.swipe && !stack.swipe.done)
                stack.commitSwipe();
            stack.swipe = null;
            stack.swiping = false;
        }
    }
    Keys.onPressed: event => {
        if (!many)
            return;
        const to = {
            [Qt.Key_Left]: index - 1,
            [Qt.Key_Right]: index + 1,
            [Qt.Key_Home]: 0,
            [Qt.Key_End]: count - 1
        }[event.key];
        if (to === undefined)
            return;
        event.accepted = true;
        go(to);
    }

    // .media-arrow: shown while hovered, gone at the ends.
    component Arrow: MediaGlass {
        id: arrow
        backdrop: picturePlane
        blurRadius: 12
        saturation: 0.5
        property int step: 1
        readonly property bool hidden: step < 0 ? stack.index === 0 : stack.index === stack.count - 1
        visible: stack.many
        z: 500
        width: 30
        height: 30
        radius: 15
        y: (stack.height - height) / 2
        color: hit.containsMouse ? Qt.rgba(44 / 255, 44 / 255, 44 / 255, 0.62) : Qt.rgba(22 / 255, 22 / 255, 22 / 255, 0.5)
        border.width: 0.5
        border.color: Qt.rgba(1, 1, 1, 0.2)
        activeFocusOnTab: !hidden
        Keys.onSpacePressed: stack.go(stack.index + step)
        Keys.onReturnPressed: stack.go(stack.index + step)
        readonly property bool shown: (stack.hover || hit.containsMouse || activeFocus) && !hidden
        opacity: shown ? 1 : 0
        scale: hit.pressed ? 0.9 : shown ? 1 : 0.85
        Behavior on opacity {
            enabled: !Theme.reducedMotion
            NumberAnimation {
                duration: 200
            }
        }
        Behavior on scale {
            enabled: !Theme.reducedMotion
            NumberAnimation {
                duration: 300
                easing.type: Easing.OutCubic
            }
        }
        Accessible.role: Accessible.Button
        Accessible.name: step < 0 ? "Previous photo" : "Next photo"
        Accessible.ignored: hidden
        Accessible.onPressAction: stack.go(stack.index + step)
        BoxShadow {
            anchors.fill: parent
            z: -1
            radius: 15
            blur: 8
            offsetY: 2
            color: Qt.rgba(0, 0, 0, 0.3)
        }
        PathIcon {
            anchors.centerIn: parent
            width: 12
            height: 12
            name: "media-chevron"
            color: Qt.rgba(1, 1, 1, 0.92)
            transform: Scale {
                origin.x: 6
                xScale: arrow.step > 0 ? -1 : 1
            }
        }
        MouseArea {
            id: hit
            anchors.fill: parent
            enabled: !arrow.hidden
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: stack.go(stack.index + arrow.step)
        }
    }
    Arrow {
        objectName: "mediaPrev"
        step: -1
        x: 10
    }
    Arrow {
        objectName: "mediaNext"
        step: 1
        x: stack.width - width - 10
    }

    // .media-dots: one per picture, or a counter past ten.
    MediaGlass {
        objectName: "mediaDots"
        backdrop: picturePlane
        visible: stack.many
        z: 500
        height: 18
        radius: 9
        width: dots.width + 14
        x: (stack.width - width) / 2
        y: stack.height - height - 10
        color: Qt.rgba(0, 0, 0, 0.3)
        Row {
            id: dots
            x: 7
            anchors.verticalCenter: parent.verticalCenter
            spacing: 5
            Repeater {
                model: stack.count <= 10 ? stack.count : 0
                delegate: Rectangle {
                    id: dot
                    required property int index
                    objectName: "mediaDot"
                    activeFocusOnTab: true
                    Keys.onSpacePressed: stack.go(index)
                    Keys.onReturnPressed: stack.go(index)
                    Accessible.onPressAction: stack.go(index)
                    readonly property real near: Math.max(0, 1 - Math.abs(stack.pos - index))
                    width: 6 + 10 * near
                    height: 6
                    radius: 3
                    color: Qt.rgba(1, 1, 1, 0.42 + near * 0.58)
                    Accessible.role: Accessible.Button
                    Accessible.name: "Photo " + (index + 1) + " of " + stack.count
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -3
                        anchors.topMargin: -6
                        anchors.bottomMargin: -6
                        cursorShape: Qt.PointingHandCursor
                        onClicked: stack.go(dot.index)
                    }
                }
            }
            Text {
                objectName: "mediaCounter"
                visible: stack.count > 10
                text: (stack.index + 1) + " / " + stack.count
                color: Qt.rgba(1, 1, 1, 0.92)
                font.pointSize: Theme.points(11.5)
                font.weight: Theme.weight(Font.DemiBold)
                font.features: {
                    "tnum": 1
                }
                lineHeight: 18
                lineHeightMode: Text.FixedHeight
            }
        }
    }
}
