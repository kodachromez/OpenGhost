import QtQuick
import OpenGhost.Native

// The model sparkle (model-button.js): a big outlined star and a small solid
// one circling it, in the 60-unit box of the other glyphs. `bigTurn` turns
// the big star about its centre; `turn` moves the small star along its orbit
// round the big one and `swell` scales it in place. Alive (the stage's mark),
// the small star keeps circling, once every 9 s.
Item {
    id: glyph
    property color color: Theme.accent
    property real bigTurn: 0
    property real bigScale: 1
    property real turn: 0
    property real swell: 1
    property bool alive: false
    property real orbit: 0
    // Where the small star stands on its orbit, in degrees.
    readonly property real stands: turn + orbit
    implicitWidth: 16
    implicitHeight: 16

    // The big star's centre (55, 63) and the small one's (77, 41) in the
    // glyph's 30…90 box.
    readonly property point bigCentre: Qt.point(width * 25 / 60, height * 33 / 60)
    readonly property point smallCentre: Qt.point(width * 47 / 60, height * 11 / 60)

    PathIcon {
        anchors.fill: parent
        name: "sparkle"
        color: glyph.color
        transform: [
            Scale {
                origin.x: glyph.bigCentre.x
                origin.y: glyph.bigCentre.y
                xScale: glyph.bigScale
                yScale: glyph.bigScale
            },
            Rotation {
                origin.x: glyph.bigCentre.x
                origin.y: glyph.bigCentre.y
                angle: glyph.bigTurn
            }
        ]
    }
    PathIcon {
        anchors.fill: parent
        name: "sparkle-small"
        color: glyph.color
        transform: [
            Scale {
                origin.x: glyph.smallCentre.x
                origin.y: glyph.smallCentre.y
                xScale: Math.max(0, glyph.swell)
                yScale: Math.max(0, glyph.swell)
            },
            Rotation {
                origin.x: glyph.bigCentre.x
                origin.y: glyph.bigCentre.y
                angle: glyph.stands
            }
        ]
    }
    NumberAnimation on orbit {
        running: glyph.alive && !Theme.reducedMotion && glyph.visible
        from: 0
        to: 360
        duration: 9000
        loops: Animation.Infinite
    }
    onAliveChanged: if (!alive) orbit = 0
}
