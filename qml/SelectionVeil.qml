import QtQuick
import OpenGhost.Cpp

// selection-focus.js: while a piece of a reply is selected, the rest of the
// feed steps back behind a soft blur, the selection cut out of it. The veil
// is the feed under backdrop-filter: blur(5px) brightness(.78)
// contrast(.949) (light: contrast(.6) brightness(1.25)), each function
// scaled by `progress` (the animation's eased value, as Chromium
// interpolates them); over the chat background the dimming leaves the
// background its own colour, so the cut-outs leave no boxes.
//
// The cut-outs are Selection.geometry()'s: per line of the range's client
// rects a band 2 px wider than the line on each side whose edges fade in
// over 56 px (a smoothstep in ten steps), 3 px taller with 5 px fades above
// and below; lines closer than 16 px join into one lit block. They are kept
// in the selected row's coordinates, so scrolling moves them with the text,
// and kept while the veil leaves, as OpenGhost's leaving veil keeps its mask.
Item {
    id: veil
    required property Flickable feed
    property real progress: 0
    // The panel's rounded inner edge: the veil stays inside it.
    property real radius: 10.5
    property real inset: 1.5
    visible: progress > 0.0005

    readonly property real brightness: 1 + ((Theme.light ? 1.25 : 0.78) - 1) * progress
    readonly property real contrast: 1 + ((Theme.light ? 0.6 : 0.949) - 1) * progress

    // Measured when the selection shows and whenever it or its row changes
    // while shown; the row they belong to.
    property var cuts: []
    property Item surface: null
    function redraw() {
        if (!Selection.shown)
            return
        surface = Selection.rowSurface()
        const geometry = Selection.geometry()
        cuts = geometry.cuts ?? []
    }
    Connections {
        target: Selection
        function onShownChanged() { if (Selection.shown) veil.redraw() }
        function onChanged() { if (Selection.shown) Qt.callLater(veil.redraw) }
        function onLayoutChanged() { if (Selection.shown) Qt.callLater(veil.redraw) }
    }
    Connections {
        target: veil.feed
        function onContentHeightChanged() { if (Selection.shown) Qt.callLater(veil.redraw) }
        function onWidthChanged() { if (Selection.shown) Qt.callLater(veil.redraw) }
    }
    readonly property point origin: {
        void veil.feed.contentY
        void veil.feed.contentHeight
        void veil.cuts
        return veil.surface ? veil.surface.mapToItem(veil, 0, 0) : Qt.point(0, 0)
    }

    // The mask: lit where the selection is cut out.
    Gradient {
        id: rampIn // smoothstep(t) at t·56 px
        orientation: Gradient.Horizontal
        GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0) }
        GradientStop { position: 0.1; color: Qt.rgba(1, 1, 1, 0.028) }
        GradientStop { position: 0.2; color: Qt.rgba(1, 1, 1, 0.104) }
        GradientStop { position: 0.3; color: Qt.rgba(1, 1, 1, 0.216) }
        GradientStop { position: 0.4; color: Qt.rgba(1, 1, 1, 0.352) }
        GradientStop { position: 0.5; color: Qt.rgba(1, 1, 1, 0.5) }
        GradientStop { position: 0.6; color: Qt.rgba(1, 1, 1, 0.648) }
        GradientStop { position: 0.7; color: Qt.rgba(1, 1, 1, 0.784) }
        GradientStop { position: 0.8; color: Qt.rgba(1, 1, 1, 0.896) }
        GradientStop { position: 0.9; color: Qt.rgba(1, 1, 1, 0.972) }
        GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 1) }
    }
    Gradient {
        id: rampOut
        orientation: Gradient.Horizontal
        GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 1) }
        GradientStop { position: 0.1; color: Qt.rgba(1, 1, 1, 0.972) }
        GradientStop { position: 0.2; color: Qt.rgba(1, 1, 1, 0.896) }
        GradientStop { position: 0.3; color: Qt.rgba(1, 1, 1, 0.784) }
        GradientStop { position: 0.4; color: Qt.rgba(1, 1, 1, 0.648) }
        GradientStop { position: 0.5; color: Qt.rgba(1, 1, 1, 0.5) }
        GradientStop { position: 0.6; color: Qt.rgba(1, 1, 1, 0.352) }
        GradientStop { position: 0.7; color: Qt.rgba(1, 1, 1, 0.216) }
        GradientStop { position: 0.8; color: Qt.rgba(1, 1, 1, 0.104) }
        GradientStop { position: 0.9; color: Qt.rgba(1, 1, 1, 0.028) }
        GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 0) }
    }
    Gradient {
        id: rise // RISE: down into the line.
        orientation: Gradient.Vertical
        GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0) }
        GradientStop { position: 0.1; color: Qt.rgba(1, 1, 1, 0.028) }
        GradientStop { position: 0.2; color: Qt.rgba(1, 1, 1, 0.104) }
        GradientStop { position: 0.3; color: Qt.rgba(1, 1, 1, 0.216) }
        GradientStop { position: 0.4; color: Qt.rgba(1, 1, 1, 0.352) }
        GradientStop { position: 0.5; color: Qt.rgba(1, 1, 1, 0.5) }
        GradientStop { position: 0.6; color: Qt.rgba(1, 1, 1, 0.648) }
        GradientStop { position: 0.7; color: Qt.rgba(1, 1, 1, 0.784) }
        GradientStop { position: 0.8; color: Qt.rgba(1, 1, 1, 0.896) }
        GradientStop { position: 0.9; color: Qt.rgba(1, 1, 1, 0.972) }
        GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 1) }
    }
    Gradient {
        id: fall // FALL: out of the line, downward.
        orientation: Gradient.Vertical
        GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 1) }
        GradientStop { position: 0.1; color: Qt.rgba(1, 1, 1, 0.972) }
        GradientStop { position: 0.2; color: Qt.rgba(1, 1, 1, 0.896) }
        GradientStop { position: 0.3; color: Qt.rgba(1, 1, 1, 0.784) }
        GradientStop { position: 0.4; color: Qt.rgba(1, 1, 1, 0.648) }
        GradientStop { position: 0.5; color: Qt.rgba(1, 1, 1, 0.5) }
        GradientStop { position: 0.6; color: Qt.rgba(1, 1, 1, 0.352) }
        GradientStop { position: 0.7; color: Qt.rgba(1, 1, 1, 0.216) }
        GradientStop { position: 0.8; color: Qt.rgba(1, 1, 1, 0.104) }
        GradientStop { position: 0.9; color: Qt.rgba(1, 1, 1, 0.028) }
        GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 0) }
    }
    Item {
        id: maskLayer
        objectName: "veilMask"
        width: veil.width
        height: veil.height
        Item {
            x: veil.origin.x
            y: veil.origin.y
            Repeater {
                model: veil.visible ? veil.cuts : []
                delegate: Item {
                    id: cut
                    required property var modelData
                    readonly property bool band: modelData.kind === "band"
                    x: modelData.x
                    y: modelData.y
                    width: modelData.w
                    height: modelData.h
                    // A band: its sides fade in over FEATHER.x; it is always
                    // wider than both together (2 × (2 + 56) px at least).
                    Rectangle {
                        visible: cut.band
                        width: 56
                        height: cut.height
                        gradient: rampIn
                    }
                    Rectangle {
                        visible: cut.band
                        x: 56
                        width: Math.max(0, cut.width - 112)
                        height: cut.height
                        color: "white"
                    }
                    Rectangle {
                        visible: cut.band
                        x: cut.width - 56
                        width: 56
                        height: cut.height
                        gradient: rampOut
                    }
                    Rectangle {
                        visible: !cut.band
                        anchors.fill: parent
                        gradient: cut.modelData.kind === "rise" ? rise : fall
                    }
                }
            }
        }
    }
    ShaderEffectSource {
        id: maskTexture
        visible: false
        live: true
        hideSource: true
        sourceItem: maskLayer
    }
    ShaderEffectSource {
        id: sharp
        visible: false
        live: true
        sourceItem: veil.visible ? veil.feed : null
        sourceRect: Qt.rect(0, 0, veil.width, veil.height)
    }
    ShaderEffect {
        id: across
        anchors.fill: parent
        property var source: sharp
        property vector2d delta: Qt.vector2d(1 / width, 0)
        property real sigma: 5 * veil.progress
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
        property real sigma: 5 * veil.progress
        property real goo: 0
        property color tint: "transparent"
        fragmentShader: "qrc:/shaders/blur.frag.qsb"
    }
    ShaderEffectSource { id: blurredTexture; sourceItem: down; hideSource: true; visible: false }
    ShaderEffect {
        objectName: "veilComposite"
        anchors.fill: parent
        property var sharp: sharp
        property var blurred: blurredTexture
        property var mask: maskTexture
        property color background: Theme.chatBg
        property real gain: veil.brightness * veil.contrast
        property real lift: 0.5 * (1 - veil.contrast) * (Theme.light ? veil.brightness : 1)
        property vector2d extent: Qt.vector2d(width, height)
        property vector4d inner: Qt.vector4d(veil.inset, veil.inset, width - 2 * veil.inset,
                                             height - 2 * veil.inset)
        property real radius: veil.radius
        fragmentShader: "qrc:/shaders/veil.frag.qsb"
    }
}
