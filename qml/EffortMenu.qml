import QtQuick
import QtQuick.Controls
import OpenGhost.Cpp

// The composer's effort dropdown, beside the model button: a plain list of
// the levels the selected model supports, as the backend reports them (Pi
// derives them from the model's own metadata), and nothing else. Hidden when
// the model has none; disabled while a run is going. A choice goes through
// Settings to the chat (its Pi takes it at once); the shown value is always
// the chat's canonical one, so a refused choice snaps back.
ComboBox {
    id: menu
    required property var settings
    property bool locked: false
    readonly property var levels: settings.levels
    readonly property string lockHint: locked ? "You can change effort when OpenGhost finishes" : ""
    // Display names only; which levels exist is always the model's.
    readonly property var names: ({ none: "Instant", off: "Off", minimal: "Minimal", low: "Low",
                                    medium: "Medium", high: "High", xhigh: "Extra high", max: "Max",
                                    ultra: "Ultra" })
    function nameOf(level) { return names[level] ?? level.charAt(0).toUpperCase() + level.slice(1) }
    function canonical() { return levels.indexOf(settings.thinking) }

    visible: count > 0
    enabled: count > 0 && !locked
    onLockedChanged: if (locked) popup.close()
    model: levels
    currentIndex: canonical()
    displayText: currentIndex >= 0 ? nameOf(levels[currentIndex]) : "Effort"
    onActivated: function(index) {
        settings.chooseThinking(levels[index])
        currentIndex = Qt.binding(canonical)
    }
    implicitHeight: 34
    font.pixelSize: 14
    font.weight: Theme.weight(500)
    hoverEnabled: true
    Accessible.name: currentIndex >= 0 ? "Effort: " + displayText : "Effort"
    Accessible.description: lockHint
    ButtonTip { text: menu.lockHint }

    readonly property color ink: enabled && (hovered || popup.visible) ? Theme.accent : Theme.muted
    background: Rectangle {
        radius: 17
        color: menu.enabled && menu.hovered ? Theme.alpha(Theme.strong, 0.06) : "transparent"
        border.width: menu.visualFocus ? 2 : 0
        border.color: Theme.alpha(Theme.strong, 0.35)
    }
    contentItem: Label {
        leftPadding: 12
        rightPadding: menu.indicator.width + 4
        verticalAlignment: Text.AlignVCenter
        text: menu.displayText
        font: menu.font
        color: menu.ink
        opacity: menu.enabled ? 1 : 0.3
        elide: Text.ElideRight
    }
    indicator: Label {
        x: menu.width - width - 10
        y: (menu.height - height) / 2
        text: "▾"
        color: menu.ink
        opacity: menu.enabled ? 1 : 0.3
        font.pixelSize: 12
    }
    delegate: ItemDelegate {
        id: row
        required property int index
        required property string modelData
        objectName: "effortOption-" + modelData
        width: ListView.view ? ListView.view.width : implicitWidth
        highlighted: menu.highlightedIndex === index
        text: menu.nameOf(modelData)
        font: menu.font
        contentItem: Label {
            text: row.text
            font: row.font
            color: row.index === menu.currentIndex ? Theme.accent : Theme.text
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 6
            color: row.highlighted || row.hovered ? Theme.rowHover : "transparent"
        }
    }
    popup: Popup {
        y: -implicitHeight - 6
        width: Math.max(menu.width, 140)
        implicitHeight: contentItem.implicitHeight + 8
        padding: 4
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: menu.popup.visible ? menu.delegateModel : null
            currentIndex: menu.highlightedIndex
        }
        background: Rectangle {
            radius: 10
            color: Theme.composerBg
            border.width: 1
            border.color: Theme.composerBorder
        }
    }
}
