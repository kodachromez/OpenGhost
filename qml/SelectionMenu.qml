import QtQuick
import QtQuick.Controls
import OpenGhost.Native

// selection-menu.js: once a reply's selection is let go, a small toolbar
// stands 8 px above its first line (below its last when that would come
// within 32 px of the view's top), from its left edge kept 8 px inside the
// view. It keeps its place in the transcript's content, so it travels with
// the text.
// It fades in over 0.16 s (ease) and grows from .94 over 0.3 s (motion
// easing) from 24 px into its bottom (top, below); it leaves the same way,
// at once with reduced motion. It is drawn over the veil (z 2 over 1 in
// OpenGhost), so it lies outside the transcript, at a place in the
// transcript's content. "Ask frontend" puts the selection, trimmed, in
// the composer as a quote and lets it go. 1.2's other action, Mini chat,
// opens a side conversation with a second agent, which Ghosty does not
// start (the Electron client says so), so the toolbar holds Ask alone, as
// 1.2's does inside a dialog. Its button takes no Tab stop (1.2's Tab goes
// to the text's links and the message's Copy) and lights in 0.15 s (ease)
// on hover.
Item {
    id: menu
    objectName: "selectionToolbar"
    required property Flickable view
    signal ask(string text)
    // Its place in the transcript's content.
    property point spot: Qt.point(0, 0)
    readonly property point at: {
        void view.contentY
        void view.contentX
        return view.contentItem.mapToItem(parent, spot.x, spot.y)
    }
    x: at.x
    y: at.y
    readonly property bool shown: Selection.shown
    property bool below: false
    width: pane.width
    height: pane.height
    z: 2
    property real fade: 0
    property real growth: 0.94
    opacity: fade
    visible: fade > 0 || growing.running
    transform: Scale {
        origin.x: 24
        origin.y: menu.below ? 0 : menu.height
        xScale: menu.growth
        yScale: menu.growth
    }
    NumberAnimation {
        id: fading
        target: menu
        property: "fade"
        duration: 160
        easing.type: Easing.Bezier
        easing.bezierCurve: Theme.ease
    }
    NumberAnimation {
        id: growing
        target: menu
        property: "growth"
        duration: 300
        easing.type: Easing.Bezier
        easing.bezierCurve: Theme.motion
    }
    function animate(fadeTo, growTo) {
        fading.stop()
        growing.stop()
        fading.to = fadeTo
        growing.to = growTo
        fading.start()
        growing.start()
    }
    // In, it always plays (1.2's .is-shown transition outranks the reduced
    // motion rule); out, it is at once with reduced motion.
    onShownChanged: {
        if (shown) {
            place()
            animate(1, 1)
        } else if (Theme.reducedMotion) {
            fading.stop()
            growing.stop()
            fade = 0
            growth = 0.94
        } else {
            animate(0, 0.94)
        }
    }

    // place(): over the first line, or under the last, inside the view.
    function place() {
        const surface = Selection.rowSurface()
        const geometry = Selection.geometry()
        if (!surface || !geometry.first || !view)
            return
        const first = surface.mapToItem(view, geometry.first)
        const last = surface.mapToItem(view, geometry.last)
        let top = first.y - height - 8
        below = top < 32
        if (below)
            top = last.y + last.height + 8
        const left = Math.min(Math.max(8, first.x), view.width - width - 8)
        spot = view.mapToItem(view.contentItem, Math.round(left), Math.round(top))
    }
    Rectangle {
        id: pane
        width: ask.width + 6
        height: 36
        radius: 13
        color: Theme.light ? "white" : Qt.rgba(36 / 255, 36 / 255, 36 / 255, 1)
        // 0 10px 30px rgba(0, 0, 0, .45 · --shadow), then the 1 px inset contour.
        BoxShadow {
            z: -1
            anchors.fill: parent
            radius: 13
            blur: 30
            offsetY: 10
            color: Theme.alpha("black", 0.45 * Theme.shadow)
        }
        Rectangle {
            anchors.fill: parent
            radius: 13
            color: "transparent"
            border.width: 1
            border.color: Theme.alpha(Theme.fg, 0.09)
        }
        AbstractButton {
            id: ask
            objectName: "askOpenGhost"
            x: 3
            y: 3
            height: 30
            width: 9 + 14 + 7 + label.implicitWidth + 11
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            Accessible.name: label.text
            background: Rectangle {
                radius: 10
                color: Theme.alpha(Theme.fg, ask.hovered || ask.visualFocus ? 0.08 : 0)
                Behavior on color { ColorAnimation { duration: 150; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
            }
            contentItem: Item {
                PathIcon {
                    x: 9 - ask.leftPadding
                    anchors.verticalCenter: parent.verticalCenter
                    width: 14
                    height: 14
                    name: "quote"
                    color: Theme.alpha(Theme.fg, 0.75)
                }
                Label {
                    id: label
                    x: 9 + 14 + 7 - ask.leftPadding
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Ask OpenGhost"
                    color: Theme.alpha(Theme.fg, 0.85)
                    font.pixelSize: 13
                    font.weight: Theme.weight(600)
                }
            }
            onClicked: {
                const text = Selection.text.trim()
                Selection.clear()
                if (text.length > 0)
                    menu.ask(text)
            }
        }
    }
}
