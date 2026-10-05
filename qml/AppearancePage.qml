import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import OpenGhost.Native

// Settings → Appearance (OpenGhost 1.2's settings-appearance.js): the themes
// as small windows of the app itself, painted with that theme's own colours.
// Picking one spreads the new theme from its window over the whole app
// (Theme.pick; ThemeReveal draws it). Arrows move round the three and pick.
Column {
    id: page
    readonly property var choices: [
        { id: "light", title: "Light" },
        { id: "dark", title: "Dark" },
        { id: "system", title: "System" }
    ]
    width: parent ? parent.width : 0

    Text {
        width: parent.width
        text: "Pick a theme. System follows your computer and switches along with it."
        font.pointSize: Theme.points(13.5)
        lineHeightMode: Text.FixedHeight
        lineHeight: 20
        topPadding: Theme.halfLeading(font, 20)
        bottomPadding: -Theme.halfLeading(font, 20)
        color: Theme.secondary
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }
    // .settings-lead's 4 px collapses into .theme-options' 22.
    Item { width: 1; height: 22 }
    // Three columns 18 px apart: a 16:10.5 window and its name 11 px below.
    Row {
        id: options
        spacing: 18
        Accessible.role: Accessible.List
        Accessible.name: "Theme"
        Repeater {
            model: page.choices
            delegate: ThemeOption {}
        }
        function step(by) {
            const at = page.choices.findIndex(c => c.id === Theme.choice)
            const next = (at + by + page.choices.length) % page.choices.length
            const item = children[next]
            item.forceActiveFocus(Qt.TabFocusReason)
            item.pick()
        }
    }

    component ThemeOption: AbstractButton {
        id: option
        required property var modelData
        required property int index
        readonly property bool chosen: Theme.choice === modelData.id
        readonly property real frameWidth: (page.width - 36) / 3
        objectName: "themeOption-" + modelData.id
        width: frameWidth
        height: frame.height + 11 + label.height
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        activeFocusOnTab: chosen
        Accessible.role: Accessible.RadioButton
        Accessible.checked: chosen
        text: modelData.title
        Keys.onRightPressed: options.step(1)
        Keys.onDownPressed: options.step(1)
        Keys.onLeftPressed: options.step(-1)
        Keys.onUpPressed: options.step(-1)
        onClicked: pick()
        // The new theme spreads from the middle of this card's window.
        function pick() {
            if (chosen)
                return
            const at = frame.mapToItem(null, frame.width / 2, frame.height / 2)
            Theme.pick(modelData.id, at.x, at.y)
        }
        background: null
        contentItem: Item {
            Item {
                id: frame
                objectName: "themeFrame-" + option.modelData.id
                // Hover lifts the window 2 px, a press sets it to .98 (0.5 s).
                property real lift: option.hovered && !option.down ? -2 : 0
                // The rings' box-shadow eases over 0.35 s.
                property real ring: option.chosen || option.visualFocus ? 4 : 1
                Behavior on ring { enabled: !Theme.reducedMotion; NumberAnimation { duration: 350; easing.type: Easing.Bezier; easing.bezierCurve: [0.25, 0.1, 0.25, 1, 1, 1] } }
                width: option.frameWidth
                height: width * 10.5 / 16
                transform: Translate { y: frame.lift }
                scale: option.down ? 0.98 : 1
                Behavior on lift { enabled: !Theme.reducedMotion; NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
                Behavior on scale { enabled: !Theme.reducedMotion; NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
                // 0 6px 16px .2 at rest; chosen, 0 10px 24px .28 (× --shadow).
                BoxShadow {
                    anchors.fill: parent
                    radius: 12
                    visible: !(option.chosen && option.visualFocus)
                    blur: option.chosen ? 24 : 16
                    offsetY: option.chosen ? 10 : 6
                    color: Theme.alpha("black", (option.chosen ? 0.28 : 0.2) * Theme.shadow)
                }
                // The rings: 1 px .1 at rest; chosen, a 2 px gap in the page's
                // colour and a 4 px accent ring (the focus ring 7 px out when
                // focused too); focused alone, the focus ring in its place.
                Rectangle {
                    visible: option.chosen && option.visualFocus
                    anchors.fill: parent
                    anchors.margins: -7
                    radius: 19
                    antialiasing: true
                    color: Theme.alpha(Theme.fg, 0.35)
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -frame.ring
                    radius: 12 + frame.ring
                    antialiasing: true
                    color: option.chosen ? Theme.accent : option.visualFocus ? Theme.alpha(Theme.fg, 0.35) : Theme.alpha(Theme.fg, 0.1)
                    Behavior on color { enabled: !Theme.reducedMotion; ColorAnimation { duration: 350 } }
                    // The gap in the page's colour (under the window at rest).
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: Math.max(12, 10 + frame.ring)
                        antialiasing: true
                        color: Theme.chatBg
                    }
                }
                // The window: its theme's preview (System: the dark one over
                // the lower right half, split on the diagonal), drawn through
                // the frame's 12 px corners.
                Item {
                    id: window
                    width: frame.width
                    height: frame.height
                    visible: false
                    layer.enabled: true
                    layer.smooth: true
                    Preview {
                        anchors.fill: parent
                        light: option.modelData.id !== "dark"
                    }
                    Item {
                        id: half
                        // Its x axis runs up the diagonal from the bottom left
                        // corner; everything below that axis is the dark half.
                        readonly property real angle: Math.atan2(-frame.height, frame.width) * 180 / Math.PI
                        visible: option.modelData.id === "system"
                        y: frame.height
                        width: Math.hypot(frame.width, frame.height)
                        height: width
                        clip: true
                        transform: Rotation { angle: half.angle }
                        Preview {
                            light: false
                            width: frame.width
                            height: frame.height
                            transform: [
                                Translate { y: -frame.height },
                                Rotation { angle: -half.angle }
                            ]
                        }
                    }
                }
                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {
                        strokeColor: "transparent"
                        fillItem: window
                        PathRectangle { width: frame.width; height: frame.height; radius: 12 }
                    }
                }
                // The chosen theme's check springs in at the top left corner.
                Item {
                    x: 7
                    y: 7
                    width: 20
                    height: 20
                    opacity: option.chosen ? 1 : 0
                    scale: option.chosen ? 1 : 0.4
                    Behavior on opacity { enabled: !Theme.reducedMotion; NumberAnimation { duration: 200 } }
                    Behavior on scale { enabled: !Theme.reducedMotion; NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: [0.34, 1.56, 0.64, 1, 1, 1] } }
                    BoxShadow {
                        anchors.fill: parent
                        radius: 10
                        blur: 8
                        offsetY: 3
                        color: Theme.alpha("black", 0.35 * Theme.shadow)
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -2
                        radius: 12
                        antialiasing: true
                        color: Theme.chatBg
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: 10
                        antialiasing: true
                        color: Theme.accent
                        PathIcon {
                            anchors.centerIn: parent
                            width: 11
                            height: 11
                            name: "check-small"
                            color: Theme.onAccent
                        }
                    }
                }
            }
            Text {
                id: label
                y: frame.height + 11
                width: option.width
                horizontalAlignment: Text.AlignHCenter
                text: option.modelData.title
                font.pointSize: Theme.points(13.5)
                font.weight: Theme.weight(500)
                lineHeightMode: Text.FixedHeight
                lineHeight: 20.25
                topPadding: Theme.halfLeading(font, 20.25)
                bottomPadding: -Theme.halfLeading(font, 20.25)
                textFormat: Text.PlainText
                color: option.chosen || option.hovered ? Theme.text : Theme.secondary
                Behavior on color { ColorAnimation { duration: Theme.reducedMotion ? 0 : 250 } }
            }
        }
    }

    // .theme-preview: the app in one theme's colours, whichever shows. The
    // sidebar's lines, then the chat panel with a bubble, the Ghost, two
    // lines of the answer and the composer with its send button.
    component Preview: Rectangle {
        id: preview
        property bool light
        readonly property var p: Theme.paletteOf(light)
        // Padding 7 % 0 0 5 % of the width; the side takes 21 % of the rest
        // and stands 5 % of it from the chat, whose lines start 13 % of it down.
        readonly property real inner: width * 0.95
        readonly property real chatX: width * 0.05 + inner * 0.26
        function ink(a) { return Qt.rgba(p.fg.r, p.fg.g, p.fg.b, a) }
        color: p.appBg
        // .theme-preview: the window's backdrop, laid out in the preview.
        Backdrop {
            anchors.fill: parent
            colors: preview.p
        }
        Column {
            x: preview.width * 0.05
            y: preview.width * 0.07 + preview.inner * 0.13
            width: preview.inner * 0.21
            spacing: 7
            Repeater {
                model: [1, 0.78, 0.88, 0.64]
                delegate: Rectangle {
                    required property real modelData
                    width: parent.width * modelData
                    height: 5
                    radius: 2.5
                    antialiasing: true
                    color: preview.ink(0.14)
                }
            }
        }
        // The chat: radius 8 at the top left, the double contour.
        Rectangle {
            x: preview.chatX - 1.5
            y: preview.width * 0.07 - 1.5
            width: preview.width - preview.chatX + 3
            height: preview.height - preview.width * 0.07 + 3
            topLeftRadius: 9.5
            antialiasing: true
            color: preview.p.contourOuter
            Rectangle {
                anchors.fill: parent
                anchors.margins: 1.5
                topLeftRadius: 8
                antialiasing: true
                color: preview.p.contourInner
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1.5
                    topLeftRadius: 6.5
                    antialiasing: true
                    color: preview.p.chatBg
                }
            }
            Item {
                id: area
                x: 1.5
                y: 1.5
                width: parent.width - 3
                height: parent.height - 3
                Rectangle {
                    x: area.width * (1 - 0.1 - 0.32)
                    y: area.height * 0.12
                    width: area.width * 0.32
                    height: area.height * 0.12
                    radius: height / 2
                    antialiasing: true
                    color: preview.p.accent
                }
                // The Ghost: 11 % wide (viewBox 60 × 62), its eyes the chat's.
                Item {
                    readonly property real w: area.width * 0.11
                    x: area.width * 0.1
                    y: area.height * 0.33
                    width: w
                    height: w * 62 / 60
                    Repeater {
                        model: ["ghost", "ghost-eyes"]
                        delegate: PathIcon {
                            required property string modelData
                            x: -parent.w / 60
                            width: parent.w * 62 / 60
                            height: width
                            name: modelData
                            color: modelData === "ghost" ? preview.p.accent : preview.p.chatBg
                        }
                    }
                }
                Rectangle {
                    x: area.width * 0.26
                    y: area.height * 0.36
                    width: area.width * 0.52
                    height: 5
                    radius: 2.5
                    antialiasing: true
                    color: preview.ink(0.2)
                }
                Rectangle {
                    x: area.width * 0.26
                    y: area.height * 0.36 + 11
                    width: area.width * 0.34
                    height: 5
                    radius: 2.5
                    antialiasing: true
                    color: preview.ink(0.2)
                }
                // The composer: a pill, 1 px border and 0 4px 10px .35 × --shadow.
                Item {
                    x: area.width * 0.09
                    y: area.height * (1 - 0.11 - 0.19)
                    width: area.width * 0.82
                    height: area.height * 0.19
                    BoxShadow {
                        anchors.fill: parent
                        radius: height / 2
                        blur: 10
                        offsetY: 4
                        color: Qt.rgba(0, 0, 0, 0.35 * preview.p.shadow)
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: height / 2
                        antialiasing: true
                        color: preview.p.composerBorder
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            radius: height / 2
                            antialiasing: true
                            color: preview.p.composerBg
                        }
                    }
                    Rectangle {
                        x: parent.width * 0.96 - width
                        y: (parent.height - height) / 2
                        width: parent.height * 0.64
                        height: width
                        radius: width / 2
                        antialiasing: true
                        color: preview.p.accent
                    }
                }
            }
        }
    }
}
