import QtQuick
import OpenGhost.Cpp

// A CSS `text-shadow: 0 0 <2σ>px <color>` of `source` (a sibling, at the
// same place): its pixels' alpha blurred by a Gaussian of `sigma` px, as
// Chromium blurs a text-shadow, across then down (shaders/blur.frag), in
// `color` at `strength`. Declared before the source it lies under it; with
// `hideSource` only the glow shows (letters drawn as nothing but their glow).
Item {
    id: glow
    required property Item source
    property real sigma: 1
    property color color: "white"
    property real strength: 1
    property bool hideSource: false
    readonly property real pad: Math.ceil(3 * sigma) + 2
    readonly property size texture: Qt.size(Math.ceil(source.width + 2 * pad),
                                            Math.ceil(source.height + 2 * pad))
    x: source.x - pad
    y: source.y - pad
    width: texture.width
    height: texture.height

    ShaderEffectSource {
        id: ink
        sourceItem: glow.source
        hideSource: glow.hideSource
        sourceRect: Qt.rect(-glow.pad, -glow.pad, glow.width, glow.height)
        textureSize: glow.texture
        visible: false
    }
    ShaderEffect {
        id: across
        anchors.fill: parent
        property var source: ink
        property vector2d delta: Qt.vector2d(1 / glow.texture.width, 0)
        property real sigma: glow.sigma
        property real goo: 0
        property vector4d tint: Qt.vector4d(0, 0, 0, 0)
        fragmentShader: "qrc:/shaders/blur.frag.qsb"
    }
    ShaderEffectSource {
        id: acrossTexture
        sourceItem: across
        hideSource: true
        textureSize: glow.texture
        visible: false
    }
    ShaderEffect {
        anchors.fill: parent
        opacity: glow.strength
        property var source: acrossTexture
        property vector2d delta: Qt.vector2d(0, 1 / glow.texture.height)
        property real sigma: glow.sigma
        property real goo: 0
        property vector4d tint: Qt.vector4d(glow.color.r, glow.color.g, glow.color.b, 1)
        fragmentShader: "qrc:/shaders/blur.frag.qsb"
    }
}
