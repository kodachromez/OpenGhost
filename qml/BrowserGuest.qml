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
    property bool documentReady: false
    property bool observingDocument: false

    // Install an observation-only document incarnation before page scripts.
    // No DOM attributes, console-message bridge or main-world globals are used.
    function observationScript(name, point, source) {
        const script = WebEngine.script()
        script.name = name
        script.worldId = WebEngineScript.ApplicationWorld
        script.injectionPoint = point
        script.runsOnSubFrames = false
        script.sourceCode = source
        return script
    }
    userScripts.collection: [
        observationScript("OpenGhost document incarnation", WebEngineScript.DocumentCreation,
            "window.__ogDocument={key:Date.now().toString(36)+'-'+Math.random().toString(36)};")
    ]
    function observeDocument() {
        if (observingDocument || !browser.automation) return
        observingDocument = true
        view.runJavaScript("JSON.stringify({key:window.__ogDocument?.key,ready:document.readyState!=='loading',url:location.href})",
                           WebEngineScript.ApplicationWorld, result => {
            observingDocument = false
            if (!result) return
            const state = JSON.parse(result)
            // An initial about:blank callback is not readiness of the requested
            // document. Redirects update the public URL before being admitted.
            if (!state.key || state.url !== view.url.toString()) return
            view.documentReady = state.ready
            browser.documentState(handle, incarnation, state.key, state.ready)
        })
    }
    Timer {
        interval: 30
        repeat: true
        running: !!view.browser.automation && (view.inLoad || !view.documentReady)
        onTriggered: view.observeDocument()
    }
    Component.onCompleted: {
        browser.attachGuest(handle, incarnation, view)
        // Qt's initial empty document predates the per-view script collection.
        // Reload it once so it has the same isolated lifecycle as all URLs.
        if (view.url.toString() === "about:blank") Qt.callLater(view.reload)
    }
    // No page-facing channel. Only the browser-owned adapter can supply code;
    // the page's main world cannot see or replace the observation/ref state.
    Connections {
        target: view.browser.automation
        function onScript(tab, generation, call, source) {
            if (tab !== view.handle || generation !== view.incarnation)
                return
            const owner = view.browser.automation
            view.runJavaScript(source, WebEngineScript.ApplicationWorld, result => {
                owner.scriptResult(tab, generation, call, result)
            })
        }
        function onNavigation(tab, generation, verb, url) {
            if (tab === view.handle && generation === view.incarnation)
                view.navigate(verb, url)
        }
    }
    function navigate(verb, url) {
        if (verb === "load") {
            if (view.url.toString() === url) view.reload()
            else view.url = url
        }
        else if (verb === "back") view.goBack()
        else if (verb === "forward") view.goForward()
        else if (verb === "reload") view.reload()
        else if (verb === "stop") view.stop()
        else if (verb === "focus") view.forceActiveFocus()
        else if (verb === "blur") view.blur()
    }

    function stopped() {
        view.inLoad = false
        browser.loadStopped(handle, incarnation, view.url.toString(), view.title)
    }
    // The keyboard goes back to the app (view.blur()).
    function blur() {
        if (view.activeFocus && view.Window.window)
            view.Window.window.contentItem.forceActiveFocus()
    }

    // DOM readiness is observed above, separately from resource loading.
    // did-fail-load fails pending readiness; native load events bracket loading.
    onLoadingChanged: info => {
        if (info.status === WebEngineLoadingInfo.LoadStartedStatus) {
            view.inLoad = true
            view.documentReady = false
            browser.loadStarted(handle, incarnation)
        } else if (info.status === WebEngineLoadingInfo.LoadSucceededStatus) {
            if (!browser.automation) browser.domReady(handle, incarnation)
            view.observeDocument()
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
            view.navigate(verb, url)
        }
        function onYieldFocus() { view.blur() }
    }
}
