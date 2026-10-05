import QtQuick
import QtQuick.Controls

// A button's name in Chromium's tooltip (TitleTip), as a title on it shows in
// OpenGhost 1.2. Declare it inside the button; `text` is usually the button's
// accessible name. The tip is named after the button: "<objectName>Tip".
HoverHandler {
    id: hover
    property string text
    onHoveredChanged: hovered ? shown.hover(hover.parent, hover.text, point.position)
                              : shown.leave(hover.parent)
    onPointChanged: if (hovered) shown.hover(hover.parent, hover.text, point.position)
    property TitleTip tip: TitleTip {
        id: shown
        objectName: hover.parent ? hover.parent.objectName + "Tip" : ""
        parent: hover.parent ? hover.parent.Overlay.overlay : null
        active: hover.parent ? hover.parent.visible : false
    }
}
