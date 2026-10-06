import QtQuick
import QtQuick.Shapes
import OpenGhost.Cpp

// --row-active-lift for the parent, which it fills: the chosen row, and
// Settings' tab glide, are each a small card on the backdrop. Dark: inset
// 0 .5px 0 rgba(255, 255, 255, .07) and 0 1px 2px rgba(0, 0, 0, .35);
// light: 0 0 0 .5px rgba(30, 48, 88, .07) and 0 1px 3px rgba(30, 48, 88, .08).
// The outer shadows lie under the parent's fill (z −1); the inset line,
// moved onto the parent, over it.
Item {
    id: lift
    property real radius: 0
    anchors.fill: parent
    z: -1

    BoxShadow {
        anchors.fill: parent
        radius: lift.radius
        offsetY: 1
        blur: Theme.light ? 3 : 2
        color: Theme.light ? Qt.rgba(30 / 255, 48 / 255, 88 / 255, 0.08) : Qt.rgba(0, 0, 0, 0.35)
    }
    Rectangle {
        visible: Theme.light
        anchors.fill: parent
        anchors.margins: -0.5
        radius: lift.radius + 0.5
        antialiasing: true
        color: Qt.rgba(30 / 255, 48 / 255, 88 / 255, 0.07)
    }
    // The dark inset: the top edge's half pixel, lit (what lies inside the
    // shape and outside it moved down .5 px), over the fill.
    Item {
        parent: lift.parent
        anchors.fill: parent
        visible: lift.visible && !Theme.light
        clip: true
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: "transparent"
                fillColor: Qt.rgba(1, 1, 1, 0.07)
                fillRule: ShapePath.OddEvenFill
                PathRectangle { width: lift.width; height: lift.height; radius: lift.radius }
                PathRectangle { y: 0.5; width: lift.width; height: lift.height; radius: lift.radius }
            }
        }
    }
}
