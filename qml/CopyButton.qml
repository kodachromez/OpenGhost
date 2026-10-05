import QtQuick
import QtQuick.Controls
import OpenGhost.Native

// OpenGhost's copy button (.md-copy): 28 px, its glyph in the secondary grey
// brightening on the control hover fill (0.2 s), a 2 px focus ring, and the
// copy glyph giving way to the green check while `copied` (fading in 0.2 s,
// from half size in 0.35 s).
ToolButton {
    id: button
    property bool copied: false
    property string tip
    width: 28
    height: 28
    padding: 0
    background: Rectangle {
        radius: 8
        color: button.hovered ? Theme.hover : "transparent"
        Behavior on color {
            enabled: !Theme.reducedMotion
            ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: 10
            visible: button.visualFocus
            color: "transparent"
            border.width: 2
            border.color: Theme.alpha(Theme.strong, 0.35)
        }
    }
    component Glyph: PathIcon {
        required property bool shown
        anchors.centerIn: parent
        width: 15
        height: 15
        opacity: shown ? 1 : 0
        scale: shown ? 1 : 0.5
        Behavior on opacity {
            enabled: !Theme.reducedMotion
            NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
        }
        Behavior on scale {
            enabled: !Theme.reducedMotion
            NumberAnimation { duration: 350; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
        }
        Behavior on color {
            enabled: !Theme.reducedMotion
            ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
        }
    }
    contentItem: Item {
        Glyph {
            shown: !button.copied
            name: "copy"
            color: button.hovered ? Theme.text : Theme.secondary
        }
        Glyph {
            shown: button.copied
            name: "check"
            color: Theme.success
        }
    }
    ToolTip.visible: hovered && tip.length > 0
    ToolTip.text: tip
}
