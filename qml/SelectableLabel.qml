import QtQuick
import OpenGhost.Native

// A short plain label that is one text of its reply's selection
// (selection.h), as a code block's language and a callout's title are in
// OpenGhost's DOM: it paints its part of the range under its glyphs.
TextEdit {
    id: label
    property var host: null // The row: key.
    property string unit: ""
    readonly property point selected: {
        void Selection.revision
        return host && host.key !== undefined && unit.length > 0 ? Selection.range(host.key, unit)
                                                                  : Qt.point(-1, -1)
    }
    function enroll() {
        if (host && host.key !== undefined && unit.length > 0)
            Selection.enroll(label, host.key, unit)
    }
    Component.onCompleted: enroll()
    onUnitChanged: enroll()
    readOnly: true
    selectByMouse: false
    activeFocusOnPress: false
    textFormat: TextEdit.PlainText
    SelectionWash {
        z: -1
        anchors.fill: parent
        target: label
        range: label.selected
        color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
    }
}
