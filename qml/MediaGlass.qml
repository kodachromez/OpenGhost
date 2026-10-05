import QtQuick
import QtQuick.Effects

// CSS backdrop-filter for media controls. Capture only the picture plane (not
// this control or other controls), with a three-sigma margin before blurring;
// crop/mask afterwards so the pill edge does not smear transparent pixels.
// Native Qt Quick effects only. Software scenegraph retains the honest tint;
// OpenGL/RHI supplies the frost, just as it does the media backdrops.
Item {
    id: glass
    required property Item backdrop
    property real blurRadius: 10
    property real saturation: 0.4 // Qt's offset: CSS saturate(1.4)
    property real radius: height / 2
    property alias color: tint.color
    property alias border: tint.border
    readonly property real padding: Math.ceil(blurRadius * 3)
    // The picture plane is a sibling. Explicit coordinate dependencies are
    // essential: mapToItem() alone does not notify on anchored geometry changes.
    // Controls scale about their centre; both siblings share parent transforms.
    readonly property point origin: backdrop
        ? Qt.point(x - backdrop.x + width * (1 - scale) / 2,
                   y - backdrop.y + height * (1 - scale) / 2)
        : Qt.point(0, 0)

    Item {
        anchors.fill: parent
        clip: true
        layer.enabled: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskThresholdMin: 0.5
            maskSpreadAtMin: 0.5
            maskSource: mask
        }
        ShaderEffectSource {
            id: sample
            visible: false
            sourceItem: glass.backdrop
            sourceRect: Qt.rect(glass.origin.x - glass.padding, glass.origin.y - glass.padding, glass.width + 2 * glass.padding, glass.height + 2 * glass.padding)
            x: -glass.padding
            y: -glass.padding
            width: glass.width + 2 * glass.padding
            height: glass.height + 2 * glass.padding
            live: true
        }
        MultiEffect {
            anchors.fill: sample
            source: sample
            blurEnabled: true
            blurMax: 64
            blur: glass.blurRadius / (0.27 * 64)
            saturation: glass.saturation
        }
    }
    Rectangle {
        id: mask
        anchors.fill: parent
        radius: glass.radius
        visible: false
        layer.enabled: true
    }
    Rectangle {
        id: tint
        anchors.fill: parent
        radius: glass.radius
        border.pixelAligned: false
        color: "transparent"
    }
}
