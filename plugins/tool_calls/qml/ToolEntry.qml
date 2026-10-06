import QtQuick
import OpenGhost.Cpp

// The Tool Calls plugin's transcript row: a tool call's card (ToolCard.qml)
// with Ghosty's tool entry motion (native-ghosty/qml/ChatEntry.qml, see
// NOTICE.md): a card rises 8 px from .98 and fades in over 0.5 s
// (approval-in), only for a row that arrived while its conversation is shown
// or as it opens (ChatEntry.arrivedAt), never one built again by scrolling or
// shown again by a renderer change. None with reduced motion.
Item {
    id: tool
    objectName: "toolEntry"
    required property Item row
    width: row.column
    implicitHeight: card.implicitHeight

    property real arrival: 1
    function arrive() {
        if (!Theme.reducedMotion)
            arriving.restart()
    }
    // Made by the row just before it arrived, or told as it does.
    Component.onCompleted: if (row.arrivedAt >= 0 && Date.now() - row.arrivedAt < 100) arrive()
    Connections {
        target: tool.row
        function onArrivedAtChanged() { tool.arrive() }
    }
    NumberAnimation {
        id: arriving
        target: tool
        property: "arrival"
        from: 0
        to: 1
        duration: 500
        easing.type: Easing.Bezier
        easing.bezierCurve: Theme.motion
    }
    Connections {
        target: Theme
        enabled: arriving.running
        function onReducedMotionChanged() { arriving.complete() }
    }

    ToolCard {
        id: card
        row: tool.row
        opacity: tool.arrival
        transform: [
            Scale {
                readonly property real at: 0.98 + 0.02 * tool.arrival
                origin.x: card.width / 2
                origin.y: card.height / 2
                xScale: at
                yScale: at
            },
            Translate { y: 8 * (1 - tool.arrival) }
        ]
    }
}
