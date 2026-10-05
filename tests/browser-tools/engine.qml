import QtQuick
import QtWebEngine
import "../../qml" as App

Window {
    id: root
    required property var browser
    width: 1400
    height: 600
    visible: true
    WebEngineProfile {
        id: profile
        offTheRecord: true
        onDownloadRequested: download => root.browser.automation.download(download)
    }
    property var activeGuest: null
    signal evaluated(var result)
    function evaluate(source, world) {
        activeGuest.runJavaScript(source, world, result => evaluated(result))
    }
    Repeater {
        model: root.browser.tabs
        delegate: Item {
            id: slot
            required property string handle
            required property bool active
            required property bool view
            required property int incarnation
            required property string source
            property int attached: 0
            property var guest: null
            anchors.fill: parent
            visible: active
            function attach() {
                const live = view && source !== "" ? incarnation : 0
                if (attached === live) return
                if (guest) { guest.destroy(); guest = null }
                attached = live
                if (live) guest = component.createObject(slot, {
                    browser: root.browser, handle: handle, incarnation: live,
                    profile: profile, url: source })
                if (active) root.activeGuest = guest
            }
            onViewChanged: Qt.callLater(attach)
            onIncarnationChanged: Qt.callLater(attach)
            onActiveChanged: if (active) root.activeGuest = guest
            Component.onCompleted: Qt.callLater(attach)
        }
    }
    // Reproduces the production driving shield: window-level events must not
    // be mistaken for targeted guest input when app chrome is above the page.
    property bool shield: false
    MouseArea {
        anchors.fill: parent
        z: 10
        visible: root.shield
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => wheel.accepted = true
    }
    // App chrome painted over the page (the chat card over a closed panel).
    property bool covered: false
    Rectangle {
        anchors.fill: parent
        z: 11
        color: "magenta"
        visible: root.covered
    }
    Component {
        id: component
        App.BrowserGuest { anchors.fill: parent }
    }
}
