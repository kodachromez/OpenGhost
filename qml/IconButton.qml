import QtQuick
import QtQuick.Controls
import OpenGhost.Cpp

// OpenGhost's icon button (icon-button.js): a square hit area, an icon in
// one opaque colour faded as a whole, and a 2 px white .35 focus ring for
// keyboard focus. `text` is its accessible name (aria-label), never drawn.
// The composer's buttons set their size and colours as composer.css does
// (--composer-button-size 34, --composer-icon-size 16).
// Its springs: a press sinks the glyph 2 units (of its 60) and fades it to
// .7 (230/27); hovering turns the glyph `turn`° or lifts it `lift` units on
// the hover spring (add-button.js, send-button.js). A press can also shrink
// the whole button by `pressScale`. With reduced motion only the press
// shows, at once.
AbstractButton {
    id: button
    property string glyph
    property real iconSize: 16
    property color iconColor: Theme.strong
    property color hoverColor: iconColor
    // --icon-opacity, --icon-hover-opacity and --icon-disabled-opacity.
    property real restOpacity: 0.55
    property real hoverOpacity: 0.85
    property real disabledOpacity: 0.25
    property color fill: "transparent"
    property real radius: 8
    // The focus ring's distance outside the button (send: 4 px).
    property real ringOutset: 0
    property real turn: 0
    property real lift: 0
    property real pressScale: 0
    property real hoverK: 230
    property real hoverC: 27
    readonly property bool lit: enabled && (hovered || visualFocus)
    implicitWidth: 34
    implicitHeight: 34
    padding: 0
    hoverEnabled: true
    scale: 1 - pressScale * pressSpring.value
    Spring { id: pressSpring; goal: button.down ? 1 : 0; k: 230; c: 27 }
    Spring {
        id: hoverSpring
        goal: !Theme.reducedMotion && button.enabled && button.hovered ? 1 : 0
        k: button.hoverK
        c: button.hoverC
    }
    background: Rectangle {
        radius: button.radius
        color: button.fill
        Rectangle {
            anchors.fill: parent
            anchors.margins: -button.ringOutset
            radius: button.radius + button.ringOutset
            visible: button.visualFocus
            color: "transparent"
            border.width: 2
            border.color: Theme.alpha(Theme.strong, 0.35)
        }
    }
    contentItem: Item {
        opacity: !button.enabled ? button.disabledOpacity
               : button.lit ? button.hoverOpacity : button.restOpacity
        Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
        PathIcon {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: (2 * pressSpring.value - button.lift * hoverSpring.value)
                                          * button.iconSize / 60
            width: button.iconSize
            height: button.iconSize
            rotation: button.turn * hoverSpring.value
            opacity: 1 - 0.3 * pressSpring.value
            name: button.glyph
            color: button.lit ? button.hoverColor : button.iconColor
            Behavior on color { ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
        }
    }
}
