import QtQuick
import QtWebEngine

// One tab's page: browser-panel.js createView()'s webview, as a Qt WebEngine
// guest in the panel's site profile (persist:browser). It reports what its
// page does to the panel's state (Browser) as the webview's events do, and
// carries out the panel's `act` commands for its tab. A guest knows nothing
// of the app beyond that: no channel, no app objects, no Node.
WebEngineView {
    id: view
    objectName: "browserGuest"
    required property var browser
    required property string handle
    // Which guest of the tab this is; reports from a replaced one are ignored.
    required property int incarnation
    property bool inLoad: false

    function stopped() {
        view.inLoad = false
        browser.loadStopped(handle, incarnation, view.url.toString(), view.title)
    }
    // The keyboard goes back to the app (view.blur()).
    function blur() {
        if (view.activeFocus && view.Window.window)
            view.Window.window.contentItem.forceActiveFocus()
    }

    // dom-ready is the first finished document; did-fail-load fails a
    // readiness still awaited; did-start/stop-loading bracket every load.
    onLoadingChanged: info => {
        if (info.status === WebEngineLoadingInfo.LoadStartedStatus) {
            view.inLoad = true
            browser.loadStarted(handle, incarnation)
        } else if (info.status === WebEngineLoadingInfo.LoadSucceededStatus) {
            browser.domReady(handle, incarnation)
            stopped()
        } else if (info.status === WebEngineLoadingInfo.LoadFailedStatus) {
            browser.loadFailed(handle, incarnation, info.errorCode, info.errorString)
            stopped()
        } else {
            stopped()
        }
    }
    // did-navigate within a load; did-navigate-in-page outside one.
    onUrlChanged: browser.navigated(handle, incarnation, view.url.toString(), !view.inLoad)
    onTitleChanged: browser.titleChanged(handle, incarnation, view.title)
    onIconChanged: browser.iconChanged(handle, incarnation, view.icon.toString())
    onCanGoBackChanged: browser.history(handle, incarnation, view.canGoBack, view.canGoForward)
    onCanGoForwardChanged: browser.history(handle, incarnation, view.canGoBack, view.canGoForward)
    // render-process-gone: the panel drops this guest; the next attempt makes another.
    onRenderProcessTerminated: browser.crashed(handle, incarnation)
    // desktop/browser.js setup(): only pointer lock and fullscreen are allowed
    // (and sanitized clipboard writes, which need no permission here).
    onPermissionRequested: permission => {
        if (permission.permissionType === WebEnginePermission.PermissionType.MouseLock)
            permission.grant()
        else
            permission.deny()
    }
    onFullScreenRequested: request => request.accept()
    // The reference's own page menu is not ported: no engine default instead.
    onContextMenuRequested: request => request.accepted = true

    Connections {
        target: view.browser
        function onAct(handle, verb, url) {
            if (handle !== view.handle)
                return
            if (verb === "load")
                view.url = url
            else if (verb === "back")
                view.goBack()
            else if (verb === "forward")
                view.goForward()
            else if (verb === "reload")
                view.reload()
            else if (verb === "stop")
                view.stop()
            else if (verb === "focus")
                view.forceActiveFocus()
            else if (verb === "blur")
                view.blur()
        }
        function onYieldFocus() { view.blur() }
    }
}
