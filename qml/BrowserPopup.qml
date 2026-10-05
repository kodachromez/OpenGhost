import QtQuick
import QtWebEngine

// desktop/browser.js setWindowOpenHandler(): a page's popup ('new-window')
// opens as its own 520×700 white window in the same site session, not as a
// panel tab. Like the reference's BrowserWindow it is no agent tool target,
// has no page menu, and its downloads are saved but neither attributed to a
// browser step nor announced; its own popups open further windows.
Window {
    id: popup
    objectName: "browserPopup"
    property alias view: page
    property alias profile: page.profile
    width: 520
    height: 700
    color: "white"
    title: page.title
    visible: true
    onClosing: popup.destroy()

    WebEngineView {
        id: page
        anchors.fill: parent
        onNewWindowRequested: request => {
            const next = Qt.createComponent("BrowserPopup.qml")
            const window = next.createObject(popup.transientParent, { profile: page.profile })
            if (window)
                request.openIn(window.view)
        }
        onWindowCloseRequested: popup.close()
        // The session's permission rule (desktop/browser.js setup) applies here too.
        onPermissionRequested: permission => {
            if (permission.permissionType === WebEnginePermission.PermissionType.MouseLock)
                permission.grant()
            else
                permission.deny()
        }
        onFullScreenRequested: request => request.accept()
        onContextMenuRequested: request => request.accepted = true
    }
}
