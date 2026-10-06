import QtQuick
import OpenGhost.Cpp

// filter: var(--veil) on an element while a reply's selection is up (the
// composer, the jump button: .is-veiled): its own pixels, shadows
// included, blurred by 5 px and then dimmed (brightness .78, contrast .949;
// light: contrast .6, brightness 1.25), each scaled by `progress`, the
// transition's eased value, as CSS interpolates filter functions. In place
// of the element while `progress` is above 0; a sibling of it.
Item {
    id: filter
    required property Item source
    property real progress: 0
    // How far the element draws past its box (its shadow), and the blur's reach.
    property real margin: 16
    visible: progress > 0.0005
    x: source.x - margin
    y: source.y - margin
    z: source.z
    width: source.width + 2 * margin
    height: source.height + 2 * margin
    // One c·gain + lift per channel: brightness then contrast (dark),
    // contrast then brightness (light).
    readonly property real brightness: 1 + ((Theme.light ? 1.25 : 0.78) - 1) * progress
    readonly property real contrast: 1 + ((Theme.light ? 0.6 : 0.949) - 1) * progress
    readonly property real gain: brightness * contrast
    readonly property real lift: 0.5 * (1 - contrast) * (Theme.light ? brightness : 1)

    ShaderEffectSource {
        id: captured
        visible: false
        live: true
        hideSource: filter.visible
        sourceItem: filter.visible ? filter.source : null
        sourceRect: Qt.rect(-filter.margin, -filter.margin, filter.width, filter.height)
    }
    ShaderEffect {
        id: across
        anchors.fill: parent
        property var source: captured
        property vector2d delta: Qt.vector2d(1 / width, 0)
        property real sigma: 5 * filter.progress
        property real goo: 0
        property color tint: "transparent"
        fragmentShader: "qrc:/shaders/blur.frag.qsb"
    }
    ShaderEffectSource { id: acrossTexture; sourceItem: across; hideSource: true; visible: false }
    ShaderEffect {
        id: down
        anchors.fill: parent
        property var source: acrossTexture
        property vector2d delta: Qt.vector2d(0, 1 / height)
        property real sigma: 5 * filter.progress
        property real goo: 0
        property color tint: "transparent"
        fragmentShader: "qrc:/shaders/blur.frag.qsb"
    }
    ShaderEffectSource { id: downTexture; sourceItem: down; hideSource: true; visible: false }
    ShaderEffect {
        anchors.fill: parent
        property var source: downTexture
        property real gain: filter.gain
        property real lift: filter.lift
        fragmentShader: "qrc:/shaders/tone.frag.qsb"
    }
}
