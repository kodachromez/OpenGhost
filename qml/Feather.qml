import QtQuick
import QtQuick.Effects

// A layer effect that fades its source out toward its edges, over
// `leftEdge`, `topEdge`, `rightEdge` and `bottomEdge` pixels, along a
// smoothstep: the CSS mask ramps of the model list and the effort stage's
// patch. MultiEffect's own mask is a threshold, not a ramp, so the source is
// drawn in bands. An opaque source (the patch) is drawn in 32 nested insets,
// each clipped, whose opacities compound over one another to the ramp's
// value in its band; corners take the nearer edge's value. A translucent one
// (the list) would build up where copies overlap, so it is drawn in
// disjoint 2 px strips instead, fading its top and bottom only.
Item {
    id: feather
    property var source
    property bool opaque: true
    property real leftEdge: 0
    property real topEdge: 0
    property real rightEdge: 0
    property real bottomEdge: 0
    readonly property int steps: 32

    // The ramp in the middle of band i.
    function ramp(i) {
        const t = (i + 0.5) / steps
        return t * t * (3 - 2 * t)
    }
    component Band: Item {
        clip: true
        MultiEffect {
            x: -parent.x
            y: -parent.y
            width: feather.width
            height: feather.height
            source: feather.source
            autoPaddingEnabled: false
        }
    }
    Repeater {
        model: feather.opaque ? feather.steps : 0
        delegate: Band {
            required property int index
            readonly property real f: index / feather.steps
            // The innermost inset is drawn in full.
            readonly property real cover: index === feather.steps - 1 ? 1 : feather.ramp(index)
            x: feather.leftEdge * f
            y: feather.topEdge * f
            width: feather.width - x - feather.rightEdge * f
            height: feather.height - y - feather.bottomEdge * f
            opacity: 1 - (1 - cover) / (1 - (index ? feather.ramp(index - 1) : 0))
        }
    }
    // Top strips, then bottom strips, 2 px each so their clips meet exactly.
    readonly property int topStrips: Math.ceil(topEdge / 2)
    readonly property int bottomStrips: Math.ceil(bottomEdge / 2)
    Repeater {
        model: feather.opaque ? 0 : feather.topStrips + feather.bottomStrips
        delegate: Band {
            required property int index
            readonly property bool atBottom: index >= feather.topStrips
            readonly property int band: atBottom ? index - feather.topStrips : index
            readonly property int count: atBottom ? feather.bottomStrips : feather.topStrips
            width: feather.width
            height: 2
            y: atBottom ? feather.height - (band + 1) * 2 : band * 2
            readonly property real t: (band + 0.5) / count
            opacity: t * t * (3 - 2 * t)
        }
    }
    Band { // Between the strips, in full.
        visible: !feather.opaque
        y: 2 * feather.topStrips
        width: feather.width
        height: Math.max(0, feather.height - 2 * (feather.topStrips + feather.bottomStrips))
    }
}
