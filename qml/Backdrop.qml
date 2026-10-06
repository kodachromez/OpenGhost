import QtQuick
import OpenGhost.Cpp

// OpenGhost 1.3's backdrop, `var(--frame), var(--app-bg)` (shaders/frame.frag):
// light falling from the left corners onto --app-bg. Laid out in `box` with
// this item at `origin` in it: the window's own size and place there for the
// window and for pieces of it pinned to the window (the sidebar's fades, a
// long title's end, as background-attachment: fixed), else its own box (the
// Settings sheet, the theme previews). `mask` cuts it (frame.frag) and
// `wash` lies over it; `radius` rounds it. `colors`, a Theme.paletteOf(), draws a theme not
// shown. The software renderer has no shaders: there it is plain --app-bg
// where unmasked, and nothing where masked.
Item {
    id: frame
    property size box: Qt.size(width, height)
    property point origin: Qt.point(0, 0)
    property var colors: null
    property int mask: 0
    property color wash: "transparent"
    property real radius: 0

    readonly property var lights: colors ? colors.frame : Theme.frame
    readonly property color base: colors ? colors.appBg : Theme.appBg
    readonly property bool shaded: GraphicsInfo.api !== GraphicsInfo.Software

    Rectangle {
        anchors.fill: parent
        visible: !frame.shaded && frame.mask === 0
        radius: frame.radius
        color: Qt.tint(frame.base, frame.wash)
    }
    ShaderEffect {
        anchors.fill: parent
        visible: frame.shaded
        property size box: frame.box
        property point origin: frame.origin
        property size size: Qt.size(width, height)
        property vector4d appBg: Qt.vector4d(frame.base.r, frame.base.g, frame.base.b, 1)
        property vector4d light0: frame.lights[0]
        property vector4d alpha0: frame.lights[1]
        property vector4d light1: frame.lights[2]
        property vector4d alpha1: frame.lights[3]
        property vector4d light2: frame.lights[4]
        property vector4d alpha2: frame.lights[5]
        property vector4d wash: Qt.vector4d(frame.wash.r, frame.wash.g, frame.wash.b, frame.wash.a)
        property real mask: frame.mask
        property real radius: frame.radius
        fragmentShader: "qrc:/shaders/frame.frag.qsb"
    }
}
