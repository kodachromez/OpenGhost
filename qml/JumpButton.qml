import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Shapes
import OpenGhost.Native

// OpenGhost's jump to the latest message (scroll-button.js, .thread-bottom):
// a 36 px round glass lens with a 17 px down arrow at .8 (1 over it). It
// springs in from 10 px lower at .6 of its size, and leaves the way it came.
// Over it the arrow sinks 5 units (of 60); a press sinks it 2 more, dims it
// by .3 and shrinks the lens by .08. The glass is the lens-bg tint and
// fill over what lies beneath it, magnified 1.15 and blurred, with the
// glass rim, inner shade and drop; liquid-glass.js's refraction at the rim
// is not drawn. `backdrop` is the item it floats over.
AbstractButton {
    id: jump
    property bool shown: false
    property Item backdrop: null
    // 0 gone, 1 in place: its lift and scale (the opacity has its own curve).
    property real rise: shown ? 1 : 0
    Behavior on rise {
        NumberAnimation {
            duration: jump.shown ? 500 : 400
            easing.type: Easing.BezierSpline
            easing.bezierCurve: jump.shown ? [0.34, 1.5, 0.64, 1, 1, 1] : [0.32, 0.72, 0, 1, 1, 1]
        }
    }
    opacity: shown ? 1 : 0
    Behavior on opacity {
        NumberAnimation {
            duration: jump.shown ? 250 : 200
            easing.type: Easing.BezierSpline
            easing.bezierCurve: [0.25, 0.1, 0.25, 1, 1, 1]
        }
    }
    visible: opacity > 0 || rise > 0
    enabled: shown
    width: 36
    height: 36
    padding: 0
    hoverEnabled: true
    text: "Jump to latest"
    transform: [
        Scale {
            origin.x: 18
            origin.y: 18
            xScale: (0.6 + 0.4 * jump.rise) * (1 - 0.08 * press.value)
            yScale: xScale
        },
        Translate { y: 10 * (1 - jump.rise) }
    ]

    // icon-button.js springs: the hover nudge and the press, [230, 27].
    Spring {
        id: nudge
        goal: jump.hovered && jump.enabled && !Theme.reducedMotion ? 1 : 0
        k: 230
        c: 27
    }
    Spring {
        id: press
        goal: jump.pressed ? 1 : 0
        k: 230
        c: 27
    }

    background: Item {
        BoxShadow { // --glass-drop
            anchors.fill: parent
            radius: 18
            blur: 10
            offsetY: 3
            color: Qt.rgba(0, 0, 0, Theme.light ? 0.12 : 0.35)
        }
        Item {
            anchors.fill: parent
            layer.enabled: true
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: disc
            }
            // The chat's background, opaque: the drop must not show through.
            Rectangle {
                anchors.fill: parent
                color: Theme.chatBg
            }
            // What lies beneath, 1.15 × larger about the centre, blurred
            // (3 px), saturated (1.6) and brightened (1.05).
            ShaderEffectSource {
                id: under
                anchors.fill: parent
                visible: false
                live: jump.visible
                sourceItem: jump.backdrop
                readonly property point at: jump.backdrop && jump.visible
                    ? jump.mapToItem(jump.backdrop, 18, 18) : Qt.point(0, 0)
                sourceRect: Qt.rect(at.x - 18 / 1.15, at.y - 18 / 1.15, 36 / 1.15, 36 / 1.15)
            }
            MultiEffect {
                anchors.fill: parent
                source: under
                visible: jump.backdrop !== null
                blurEnabled: true
                blurMax: 16
                blur: 0.7
                saturation: 0.6
                brightness: 0.005
            }
            Rectangle { // --lens-bg
                anchors.fill: parent
                color: Theme.light ? Qt.rgba(1, 1, 1, 0.6) : Qt.rgba(24 / 255, 24 / 255, 24 / 255, 0.55)
            }
            Rectangle { // --glass-fill
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0; color: Qt.rgba(1, 1, 1, Theme.light ? 0.85 : 0.1) }
                    GradientStop { position: 0.55; color: Qt.rgba(1, 1, 1, Theme.light ? 0.55 : 0.02) }
                    GradientStop { position: 1; color: Qt.rgba(1, 1, 1, Theme.light ? 0.7 : 0.05) }
                }
            }
        }
        Rectangle {
            id: disc
            anchors.fill: parent
            radius: 18
            visible: false
            layer.enabled: true
        }
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath { // --glass-shade: inset 0 1px 1px white .22, inset 0 -1px 2px black .25
                // (.9 and .06 in the light theme)
                strokeColor: "transparent"
                fillRule: ShapePath.OddEvenFill
                fillGradient: LinearGradient {
                    x1: 0; y1: 0; x2: 0; y2: 36
                    GradientStop { position: 0; color: Qt.rgba(1, 1, 1, Theme.light ? 0.9 : 0.22) }
                    GradientStop { position: 0.12; color: "transparent" }
                    GradientStop { position: 0.85; color: "transparent" }
                    GradientStop { position: 1; color: Qt.rgba(0, 0, 0, Theme.light ? 0.06 : 0.25) }
                }
                PathRectangle { x: 0; y: 0; width: 36; height: 36; radius: 18 }
                PathRectangle { x: 0; y: 1; width: 36; height: 33.5; radius: 16.75 }
            }
            ShapePath { // --glass-rim: 1 px, lit from the top left (155°).
                strokeColor: "transparent"
                fillRule: ShapePath.OddEvenFill
                fillGradient: LinearGradient {
                    x1: 18 - 0.4226 * 20.7; y1: 18 - 0.9063 * 20.7
                    x2: 18 + 0.4226 * 20.7; y2: 18 + 0.9063 * 20.7
                    GradientStop { position: 0; color: Qt.rgba(1, 1, 1, Theme.light ? 1 : 0.7) }
                    GradientStop { position: 0.32; color: Theme.light ? Qt.rgba(0, 0, 0, 0.1) : Qt.rgba(1, 1, 1, 0.14) }
                    GradientStop { position: 0.62; color: Theme.light ? Qt.rgba(0, 0, 0, 0.06) : Qt.rgba(1, 1, 1, 0.05) }
                    GradientStop { position: 1; color: Qt.rgba(1, 1, 1, Theme.light ? 0.8 : 0.38) }
                }
                PathRectangle { x: 0; y: 0; width: 36; height: 36; radius: 18 }
                PathRectangle { x: 1; y: 1; width: 34; height: 34; radius: 17 }
            }
        }
        // Keyboard focus: a 2 px ring 3 px outside.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: height / 2
            visible: jump.visualFocus
            color: "transparent"
            border.width: 2
            border.color: Theme.alpha(Theme.strong, 0.35)
        }
    }
    contentItem: Item {
        opacity: jump.hovered || jump.visualFocus ? 1 : 0.8
        Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
        PathIcon {
            // Chromium places the centred icon on whole pixels, rounding up.
            x: Math.ceil((parent.width - width) / 2)
            y: Math.ceil((parent.height - height) / 2) + (5 * nudge.value + 2 * press.value) * 17 / 60
            width: 17
            height: 17
            opacity: 1 - 0.3 * press.value
            name: "arrow-down"
            color: Theme.strong
        }
    }
}
