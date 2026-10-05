import QtQuick
import QtQuick.Controls
import QtWebChannel
import QtWebEngine

// One tab's page: browser-panel.js createView()'s webview, as a Qt WebEngine
// guest in the panel's site profile (persist:browser). It reports what its
// page does to the panel's state (Browser) as the webview's events do, and
// carries out the panel's `act` commands for its tab. A guest knows nothing
// of the app beyond that: no app objects, no Node, and no channel its page
// can reach (the sign-in hint's channel lives only in the isolated world).
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
            "window.__ogDocument={key:Date.now().toString(36)+'-'+Math.random().toString(36)};"),
        channelScript(),
        observationScript("OpenGhost sign-in hint", WebEngineScript.DocumentCreation, signInHint)
    ]
    function channelScript() {
        const script = observationScript("QWebChannel", WebEngineScript.DocumentCreation, "")
        script.sourceUrl = "qrc:///qtwebchannel/qwebchannel.js"
        return script
    }
    // desktop/browser-preload.js: only the site's host name leaves the page,
    // and only when a filled password field is being sent. It runs in the
    // isolated world, the only world with the channel: page scripts can
    // neither see the channel nor report a hint for another site.
    readonly property string signInHint: "(()=>{let told=false,channel=null,waiting='';"
        + "new QWebChannel(qt.webChannelTransport,ch=>{channel=ch;"
        + "if(waiting)channel.objects.openghost.submitted(waiting);});"
        + "const check=()=>{if(told)return;"
        + "const filled=[...document.querySelectorAll('input[type=\"password\"]')]"
        + ".some(input=>input.value.length>0);if(!filled)return;told=true;"
        + "if(channel)channel.objects.openghost.submitted(location.hostname);"
        + "else waiting=location.hostname;};"
        + "addEventListener('submit',check,true);"
        + "addEventListener('keydown',e=>{if(e.key==='Enter')check();},true);"
        + "addEventListener('click',e=>{if(e.target instanceof Element&&"
        + "e.target.closest('button, [role=\"button\"], input[type=\"submit\"]'))check();},true);})()"
    QtObject {
        id: signIn
        WebChannel.id: "openghost"
        function submitted(host) {
            view.browser.signedIn(view.handle, view.incarnation, String(host))
        }
    }
    webChannel: WebChannel { registeredObjects: [signIn] }
    webChannelWorld: WebEngineScript.ApplicationWorld
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
    // A full-page screenshot (QtBrowserAutomation::capture) briefly gives the
    // page the capture height. The slot shows a frozen copy of the page's
    // pixels meanwhile, so the visible panel never changes; both are undone
    // before the result is delivered, and when the guest goes away.
    property Item captureCover: null
    property var captureFill: undefined
    function freezeCapture() {
        if (captureCover || !view.parent)
            return false
        captureCover = coverComponent.createObject(view.parent, {
            x: view.x, y: view.y, z: view.z + 1, width: view.width, height: view.height })
        return !!captureCover
    }
    function stretchCapture(height) {
        if (captureFill === undefined)
            captureFill = view.anchors.fill
        const width = view.width
        view.anchors.fill = undefined
        view.width = width
        view.height = height
    }
    function unstretchCapture() {
        if (captureFill === undefined)
            return
        view.anchors.fill = captureFill
        captureFill = undefined
    }
    function thawCapture() {
        if (captureCover)
            captureCover.destroy()
        captureCover = null
    }
    Component.onDestruction: {
        thawCapture()
        if (inspector)
            inspector.destroy()
    }
    Component {
        id: coverComponent
        ShaderEffectSource {
            objectName: "browserCaptureCover"
            sourceItem: view
            sourceRect: Qt.rect(0, 0, width, height)
            live: false
            hideSource: false
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

    // desktop/browser.js setWindowOpenHandler(): a popup ('new-window') gets
    // its own window in this session; a tab the page asks for opens in the
    // panel next to this one (background or not); anything else is refused.
    onNewWindowRequested: request => {
        const to = request.destination
        if (to === WebEngineNewWindowRequest.InNewWindow || to === WebEngineNewWindowRequest.InNewDialog) {
            const popup = popupComponent.createObject(view.Window.window, { profile: view.profile })
            if (popup)
                request.openIn(popup.view)
        } else if (/^(https?|file):/i.test(request.requestedUrl.toString()))
            browser.openFrom(handle, incarnation, request.requestedUrl.toString(),
                             to === WebEngineNewWindowRequest.InNewBackgroundTab)
    }
    Component {
        id: popupComponent
        BrowserPopup {}
    }

    // desktop/browser.js context-menu: the page's menu, built by Browser.menu.
    onContextMenuRequested: request => {
        request.accepted = true
        pageMenu.show({
            link: request.linkUrl.toString(),
            image: request.mediaType === ContextMenuRequest.MediaTypeImage ? request.mediaUrl.toString() : "",
            editable: request.isContentEditable,
            canCut: !!(request.editFlags & ContextMenuRequest.CanCut),
            canCopy: !!(request.editFlags & ContextMenuRequest.CanCopy),
            canPaste: !!(request.editFlags & ContextMenuRequest.CanPaste),
            selection: request.selectedText,
            back: view.canGoBack,
            forward: view.canGoForward
        }, request.position)
    }
    Menu {
        id: pageMenu
        objectName: "browserPageMenu"
        property var context: ({})
        function show(context, at) {
            pageMenu.context = context
            while (count > 0)
                takeItem(0).destroy()
            for (const row of view.browser.menu(context)) {
                if (row.separator)
                    addItem(separatorComponent.createObject(null))
                else
                    addItem(itemComponent.createObject(null, {
                        text: row.label, enabled: row.enabled, verb: row.action }))
            }
            popup(at.x, at.y)
        }
    }
    Component {
        id: itemComponent
        MenuItem {
            property string verb
            onTriggered: view.pageAction(verb)
        }
    }
    Component {
        id: separatorComponent
        MenuSeparator {}
    }
    function pageAction(action) {
        const context = pageMenu.context
        if (action === "openLink")
            browser.openFrom(handle, incarnation, context.link, false)
        else if (action === "openImage")
            browser.openFrom(handle, incarnation, context.image, false)
        else if (action === "copyLink")
            view.triggerWebAction(WebEngineView.CopyLinkToClipboard)
        else if (action === "copyImage")
            view.triggerWebAction(WebEngineView.CopyImageToClipboard)
        else if (action === "cut")
            view.triggerWebAction(WebEngineView.Cut)
        else if (action === "copy")
            view.triggerWebAction(WebEngineView.Copy)
        else if (action === "paste")
            view.triggerWebAction(WebEngineView.Paste)
        else if (action === "selectAll")
            view.triggerWebAction(WebEngineView.SelectAll)
        else if (action === "back")
            view.goBack()
        else if (action === "forward")
            view.goForward()
        else if (action === "reload")
            view.reload()
        else if (action === "inspect")
            inspect()
    }
    // Inspect: the engine's in-process developer tools for the user, in their
    // own window (webContents.inspectElement). No remote-debugging endpoint;
    // automation never uses them.
    property Window inspector: null
    function inspect() {
        if (!inspector) {
            inspector = inspectorComponent.createObject(view.Window.window, { profile: view.profile })
            view.devToolsView = inspector.tools
        }
        inspector.show()
        inspector.raise()
        view.triggerWebAction(WebEngineView.InspectElement)
    }
    Component {
        id: inspectorComponent
        Window {
            property alias tools: tools
            property alias profile: tools.profile
            width: 900
            height: 640
            title: "Developer Tools"
            onClosing: {
                view.devToolsView = null
                view.inspector = null
                destroy()
            }
            WebEngineView {
                id: tools
                anchors.fill: parent
            }
        }
    }

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
