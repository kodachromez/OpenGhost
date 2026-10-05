import QtQuick
import QtQuick.Controls
import OpenGhost.Native

// The composer's model button (model-button.js, .composer-model): the
// sparkle in the composer's grey, the accent on hover, keyboard focus and
// while the stage is open. On hover the big star turns a quarter and the
// small one hops along its orbit, swelling on the way; now and then the small
// star twinkles on its own. A press sinks the glyph 2 px and fades it to .7.
// While the stage is open its glyph is `away`: the stage flies it out and
// back, and land() settles it with a small bounce.
AbstractButton {
    id: button
    property bool expanded: false
    property bool away: false
    // The chosen model's name, or what Settings asks for (its accessible name).
    property string displayText
    readonly property Item glyph: glyph
    // Where the small star stands, so a flying copy can start from there.
    readonly property real turn: glyph.turn
    readonly property bool shown: Window.window ? Window.window.visible : false
    implicitWidth: 34
    implicitHeight: 34
    padding: 0
    hoverEnabled: true
    Accessible.name: "Model: " + displayText

    function land() {
        if (Theme.reducedMotion)
            return
        bounce.restart()
    }

    // icon-button.js springs: press [230, 27], hint [170, 15], twinkle [300, 14].
    Spring { id: press; goal: button.pressed ? 1 : 0; k: 230; c: 27 }
    Spring { id: hint; goal: Theme.reducedMotion ? 0 : Math.max(button.enabled && button.hovered ? 1 : 0, button.expanded ? 1 : 0); k: 170; c: 15 }
    Spring { id: twinkle; goal: 0; k: 300; c: 14 }
    Timer {
        id: idle
        interval: 7000 + Math.random() * 8000
        running: true
        onTriggered: {
            if (button.enabled && !button.expanded && !button.hovered && button.shown
                && !Theme.reducedMotion) {
                twinkle.goal = 1
                twinkleOff.restart()
            }
            interval = 7000 + Math.random() * 8000
            restart()
        }
    }
    Timer { id: twinkleOff; interval: 240; onTriggered: twinkle.goal = 0 }

    background: Rectangle {
        radius: 8
        color: "transparent"
        visible: button.visualFocus
        border.width: 2
        border.color: Theme.alpha(Theme.strong, 0.35)
    }
    contentItem: Item {
        opacity: button.enabled ? 1 : 0.3
        ModelGlyph {
            id: glyph
            readonly property real h: hint.value
            readonly property real t: twinkle.value
            anchors.centerIn: parent
            anchors.verticalCenterOffset: 2 * press.value
            width: 16
            height: 16
            visible: !button.away
            opacity: 1 - 0.3 * press.value
            color: button.expanded || button.enabled && (button.hovered || button.visualFocus)
                   ? Theme.accent : Theme.muted
            Behavior on color { ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
            bigTurn: 90 * h
            bigScale: 1 - 0.08 * t
            turn: -28 * h - 26 * t
            swell: 1 + 0.45 * Math.sin(Math.PI * Math.max(0, Math.min(1, h))) + 0.6 * t
            transform: Scale {
                id: bounceScale
                origin.x: 8
                origin.y: 8
            }
        }
    }
    // The glyph back in the button: scale .82 → 1 on the spring curve.
    NumberAnimation {
        id: bounce
        target: bounceScale
        properties: "xScale,yScale"
        from: 0.82
        to: 1
        duration: 460
        easing.type: Easing.Bezier
        easing.bezierCurve: [0.34, 1.56, 0.64, 1, 1, 1]
    }
}
