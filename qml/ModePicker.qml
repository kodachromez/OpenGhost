import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import OpenGhost.Cpp

// The agent mode in the composer (OpenGhost 1.3 mode-picker.js,
// .composer-mode): the current mode's icon and name. Pressed, it gives way
// to a dock of the modes grown out of it (ModeDock); as the dock closes the
// current mode's icon goes home and the button comes back around it.
//
// The modes are OpenGhost's (WindowController::modes): the button shows the
// permission mode OpenGhost confirmed, the dock offers the modes OpenGhost lists
// that it can draw, and a pick asks OpenGhost to change only the permission
// mode. Nothing here decides what a mode allows.
AbstractButton {
    id: picker
    objectName: "modePicker"
    // WindowController::modes.
    property var modes: ({})
    // The dock it opens (Main.qml); the button lends it its place.
    property Item dock: null
    // The dock is out: the button has turned into it.
    property bool away: false

    // mode-picker.js MODES with i18n.js's names and hints. Full's hint
    // says what OpenGhost's Full does: it never asks, within its backend authority.
    readonly property var known: ({
        ask: {icon: "lock", name: "Ask", hint: "Asks before running commands, changing files or going online", tone: ""},
        auto: {icon: "shield", name: "Auto", hint: "Works in the project folder on its own, asks before risky steps", tone: ""},
        full: {icon: "shield-alert", name: "Full access", hint: "Never asks. Can do anything OpenGhost is allowed to", tone: "warn"}
    })
    // The modes to offer: those OpenGhost lists, in its order, that are drawn here.
    readonly property var choices: (modes.permissions ?? []).filter(id => known[id] !== undefined)
                                       .map(id => Object.assign({id: id}, known[id]))
    readonly property string mode: modes.permission ?? ""
    readonly property var shown: known[mode] ?? null
    // The icons' box, which the dock grows out of and the current icon goes home to.
    readonly property Item icons: iconBox

    visible: modes.known === true && shown !== null && choices.length > 0
    height: 34 // --composer-button-size
    implicitWidth: 10 + 16 + 7 + label.implicitWidth + 12
    width: implicitWidth
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    Accessible.role: Accessible.ButtonMenu
    Accessible.name: shown ? "Agent mode: " + shown.name : "Agent mode"
    Accessible.description: shown ? shown.hint : ""
    ButtonTip { text: picker.shown ? picker.shown.hint : "" }
    HoverHandler { cursorShape: Qt.PointingHandCursor }

    // A key presses it as a click does (Space, or Enter as Space:
    // ButtonKeys) and opens the dock with the keyboard's focus.
    property bool byKey: false
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Space && !event.isAutoRepeat)
            byKey = true
    }
    onClicked: {
        if (dock)
            dock.toggle(byKey)
        byKey = false
    }
    onCanceled: byKey = false

    // The name changing: the button resizes (380 ms) and the name comes out
    // of a 4 px blur from 3 px down (340 ms), both on the motion curve.
    property bool first: true
    onModeChanged: {
        if (first || Theme.reducedMotion) {
            first = mode.length === 0
            return
        }
        labelIn.play()
    }
    Behavior on width {
        enabled: !picker.first && !Theme.reducedMotion
        NumberAnimation { duration: 380; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
    }
    Entrance { id: labelIn; duration: 340 }

    // The dock's return: the icon settles in with a small bounce (460 ms
    // from .82 on cubic-bezier(.34, 1.56, .64, 1)).
    Entrance { id: land; duration: 460; curve: [0.34, 1.56, 0.64, 1, 1, 1] }
    function home() {
        away = false
        land.play()
    }

    // .composer-mode.is-away: gone to the dock (opacity .16 s, blur .2 s);
    // back in .34 s and .4 s.
    property real presence: away ? 0 : 1
    Behavior on presence {
        enabled: !Theme.reducedMotion
        NumberAnimation { duration: picker.away ? 160 : 340; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
    }
    property real sharp: away ? 0 : 1
    Behavior on sharp {
        enabled: !Theme.reducedMotion
        NumberAnimation { duration: picker.away ? 200 : 400; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
    }
    opacity: presence
    layer.enabled: sharp < 1
    layer.effect: MultiEffect {
        blurEnabled: true
        blurMax: 16
        autoPaddingEnabled: true
        blur: Math.min(1, 3 * (1 - picker.sharp) / (0.27 * 16))
    }

    // Hover: fg .06 behind it and the accent (0.2 s).
    readonly property color ink: hovered && !away ? Theme.accent : Theme.muted
    property color shownInk: ink
    Behavior on shownInk { ColorAnimation { duration: 200 } }
    background: Rectangle {
        radius: 17
        color: picker.hovered && !picker.away ? Theme.alpha(Theme.strong, 0.06) : "transparent"
        Behavior on color { ColorAnimation { duration: 200 } }
        // :focus-visible: a 2 px ring at its edge.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: height / 2
            visible: picker.visualFocus
            color: "transparent"
            border.width: 2
            border.color: Theme.alpha(Theme.strong, 0.35)
        }
    }
    contentItem: Item {
        // .composer-mode-icons: every mode's icon in one 16 px box; the
        // current one shows (opacity .22 s; scale and turn .45 s on
        // cubic-bezier(.34, 1.4, .64, 1)), Full's in the warning colour.
        Item {
            id: iconBox
            objectName: "modeIcons"
            x: 10
            y: 9
            width: 16
            height: 16
            scale: 0.82 + 0.18 * land.value
            Repeater {
                model: Object.keys(picker.known)
                PathIcon {
                    id: modeIcon
                    required property string modelData
                    readonly property bool current: modelData === picker.mode
                    objectName: "modeIcon-" + modelData
                    anchors.fill: parent
                    name: picker.known[modelData].icon
                    color: picker.mode === "full" ? Theme.warn : picker.shownInk
                    property real on: current ? 1 : 0
                    Behavior on on {
                        enabled: !Theme.reducedMotion
                        NumberAnimation { duration: 450; easing.type: Easing.Bezier; easing.bezierCurve: [0.34, 1.4, 0.64, 1, 1, 1] }
                    }
                    property real seen: current ? 1 : 0
                    Behavior on seen {
                        enabled: !Theme.reducedMotion
                        NumberAnimation { duration: 220; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
                    }
                    opacity: seen
                    scale: 0.6 + 0.4 * on
                    rotation: -12 * (1 - on)
                }
            }
        }
        Label {
            id: label
            objectName: "modeLabel"
            x: 10 + 16 + 7
            height: 34
            verticalAlignment: Text.AlignVCenter
            text: picker.shown ? picker.shown.name : ""
            color: picker.shownInk
            font.pixelSize: 14
            font.weight: Theme.weight(500)
            opacity: labelIn.value
            transform: Translate { y: 3 * (1 - labelIn.value) }
            layer.enabled: labelIn.value < 1
            layer.effect: MultiEffect {
                blurEnabled: true
                blurMax: 16
                autoPaddingEnabled: true
                blur: Math.min(1, 4 * (1 - labelIn.value) / (0.27 * 16))
            }
        }
    }
}
