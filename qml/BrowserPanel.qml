import QtQuick
import QtQuick.Controls
import QtWebEngine
import OpenGhost.Native

// The built-in browser (browser-panel.js, styles.css .browser): a card beside
// the chat with the tab strip, the address bar and the stage holding the
// tabs' pages, its empty, loading and failure states, and the agent's
// driving and the user's control overlays. Closed, it waits under the chat
// card at .97 and takes no input; its pages stop showing unless the agent
// is using it. All state is the host's (Browser); this draws it.
Item {
    id: root
    objectName: "browserPanel"
    required property var browser
    required property var frontend
    // fit()'s room: the window less the sidebar and three gaps.
    property real room
    property bool resizing: false
    readonly property bool open: browser.open
    readonly property color pillHover: Theme.light ? "black" : "white"
    readonly property color glass: Theme.light ? Qt.rgba(1, 1, 1, 0.82) : Qt.rgba(22 / 255, 22 / 255, 22 / 255, 0.82)
    enabled: open // inert while closed

    // The sites' profile (persist:browser): persistent storage, and the
    // reference's plain Chrome user agent on this engine's Chromium version.
    WebEngineProfilePrototype {
        id: prototype
        storageName: "browser"
        persistentStoragePath: root.browser.storagePath
        cachePath: root.browser.storagePath + "/cache"
    }
    property WebEngineProfile profile: null
    // Made once, when the first guest needs it (the prototype is complete then).
    function siteProfile() {
        if (!profile) {
            const made = prototype.instance()
            const chrome = /Chrome\/([\d.]+)/.exec(made.httpUserAgent)
            made.httpUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                + "(KHTML, like Gecko) Chrome/" + (chrome ? chrome[1] : "0") + " Safari/537.36"
            // will-download: the host picks the file and attributes the download.
            made.downloadRequested.connect(download => root.browser.automation.download(download))
            profile = made
        }
        return profile
    }

    property real shown: open ? 1 : 0
    Behavior on shown {
        enabled: !Theme.reducedMotion && !root.resizing
        NumberAnimation { duration: 600; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
    }
    transform: Scale {
        origin.x: root.width
        origin.y: root.height / 2
        xScale: 0.97 + 0.03 * root.shown
        yScale: 0.97 + 0.03 * root.shown
    }

    // .browser-resize: the gap left of the card; a grip shows under the pointer.
    Item {
        id: grip
        objectName: "browserResize"
        x: -11
        y: 12
        z: 2
        width: 14
        height: root.height - 24
        Rectangle {
            readonly property bool lit: drag.containsMouse || root.resizing
            anchors.centerIn: parent
            width: 3
            height: lit ? 56 : 36
            radius: 2
            color: Theme.alpha(Theme.fg, 0.28)
            opacity: lit ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 200 } }
            Behavior on height { NumberAnimation { duration: 300; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
        }
        MouseArea {
            id: drag
            property real startX
            property real startWidth
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SplitHCursor
            onPressed: mouse => {
                startX = mapToItem(null, mouse.x, 0).x
                startWidth = root.width
                root.resizing = true
            }
            onPositionChanged: mouse => {
                if (pressed)
                    root.browser.resize(root.room, startWidth + startX - mapToItem(null, mouse.x, 0).x)
            }
            onReleased: { root.resizing = false; root.browser.save() }
            onCanceled: { root.resizing = false; root.browser.save() }
        }
    }

    // .browser-card: the chat card's background, contour and lift.
    BoxShadow {
        readonly property real spread: Theme.light ? 20 : 18
        x: spread
        y: spread
        width: root.width - 2 * spread
        height: root.height - 2 * spread
        offsetY: 18
        blur: Theme.light ? 44 : 48
        color: Theme.light ? Qt.rgba(30 / 255, 48 / 255, 88 / 255, 0.16) : Qt.rgba(0, 0, 0, 0.7)
    }
    BoxShadow {
        visible: Theme.light
        width: root.width
        height: root.height
        radius: 12
        offsetY: 1
        blur: 2
        color: Qt.rgba(30 / 255, 48 / 255, 88 / 255, 0.04)
    }
    Rectangle {
        x: -1.5
        y: -1.5
        width: root.width + 3
        height: root.height + 3
        radius: 13.5
        antialiasing: true
        color: Theme.contourOuter
    }
    Rectangle {
        id: card
        anchors.fill: parent
        radius: 12
        antialiasing: true
        color: Theme.contourInner
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1.5
            radius: 10.5
            antialiasing: true
            color: Theme.chatBg
        }
    }

    // A .browser-icon: 30 px, its 18 px glyph secondary, primary on hover.
    component BarButton: AbstractButton {
        id: barButton
        property string glyph
        implicitWidth: 30
        implicitHeight: 30
        padding: 0
        hoverEnabled: true
        Accessible.name: text
        scale: down ? 0.9 : 1
        Behavior on scale { NumberAnimation { duration: 200 } }
        ButtonTip { text: barButton.text }
        background: Rectangle {
            radius: 8
            color: barButton.enabled && barButton.hovered ? Theme.alpha(Theme.fg, 0.06) : "transparent"
            Behavior on color { ColorAnimation { duration: 200 } }
            Rectangle {
                anchors.fill: parent
                radius: 8
                visible: barButton.visualFocus
                color: "transparent"
                border.width: 2
                border.color: Theme.alpha(Theme.fg, 0.35)
            }
        }
        contentItem: Item {
            PathIcon {
                anchors.centerIn: parent
                width: 18
                height: 18
                name: barButton.glyph
                color: !barButton.enabled ? Theme.tertiary
                     : barButton.hovered ? Theme.text : Theme.secondary
                Behavior on color { ColorAnimation { duration: 200 } }
            }
        }
    }
    // A .browser-pill: the bubble's colours, 13 px semibold.
    component Pill: AbstractButton {
        id: pill
        property real size: 30
        property real fontSize: 13
        implicitHeight: size
        implicitWidth: label.implicitWidth + 2 * (size === 24 ? 11 : size === 40 ? 20 : 14)
        hoverEnabled: true
        Accessible.name: text
        scale: down ? 0.96 : 1
        Behavior on scale { NumberAnimation { duration: 200 } }
        background: Rectangle {
            radius: pill.size / 2
            color: pill.hovered ? root.pillHover : Theme.accent
            Behavior on color { ColorAnimation { duration: 200 } }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -2
                radius: pill.size / 2 + 2
                visible: pill.visualFocus
                color: "transparent"
                border.width: 2
                border.color: Theme.alpha(Theme.fg, 0.35)
            }
        }
        contentItem: Text {
            id: label
            text: pill.text
            font.pixelSize: pill.fontSize
            font.weight: 600
            color: Theme.onAccent
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    // .browser-tabs: the strip, then New tab right after it.
    Item {
        id: strip
        x: 8
        y: 7
        width: root.width - 16
        height: 32
        readonly property real room: width - 4 - 30
        readonly property real tabWidth: Math.max(54, Math.min(196, (room - 3 * Math.max(0, tabList.count - 1))
                                                               / Math.max(1, tabList.count)))
        Item {
            id: listBox
            y: 1
            width: Math.min(strip.room, tabList.count * strip.tabWidth + 3 * Math.max(0, tabList.count - 1))
            height: 30
            clip: true
            Row {
                spacing: 3
                Repeater {
                    id: tabList
                    objectName: "browserTabs"
                    model: root.browser.tabs
                    delegate: Rectangle {
                        id: tabItem
                        objectName: "browserTab"
                        required property string handle
                        required property string label
                        required property string tooltip
                        required property string icon
                        required property bool loading
                        required property bool active
                        required property int index
                        readonly property color ink: active ? Theme.text : Theme.secondary
                        width: strip.tabWidth
                        height: 30
                        radius: 9
                        color: active ? Theme.alpha(Theme.fg, 0.08)
                             : tabHover.hovered ? Theme.alpha(Theme.fg, 0.04) : "transparent"
                        Behavior on color { ColorAnimation { duration: 200 } }
                        Accessible.role: Accessible.PageTab
                        Accessible.name: label
                        Accessible.selected: active
                        HoverHandler { id: tabHover }
                        ButtonTip { text: tabItem.tooltip }
                        // A new tab rises in (260 ms).
                        opacity: 0
                        transform: Translate { id: rise; y: 4 }
                        ParallelAnimation {
                            id: entrance
                            NumberAnimation { target: tabItem; property: "opacity"; to: 1; duration: 260; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                            NumberAnimation { target: rise; property: "y"; to: 0; duration: 260; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                            NumberAnimation { target: tabItem; property: "scale"; from: 0.96; to: 1; duration: 260; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                        }
                        Component.onCompleted: {
                            if (Theme.reducedMotion) {
                                opacity = 1
                                rise.y = 0
                            } else {
                                entrance.start()
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                            onClicked: mouse => {
                                if (mouse.button === Qt.MiddleButton)
                                    root.browser.close(tabItem.handle)
                                else
                                    root.browser.select(tabItem.handle)
                            }
                        }
                        Item {
                            id: tabIcon
                            x: 10
                            anchors.verticalCenter: parent.verticalCenter
                            width: 15
                            height: 15
                            // .browser-spinner
                            Rectangle {
                                visible: tabItem.loading
                                anchors.centerIn: parent
                                width: 12
                                height: 12
                                radius: 6
                                color: "transparent"
                                border.width: 1.6
                                border.color: Theme.alpha(Theme.fg, 0.18)
                                Rectangle {
                                    width: 12
                                    height: 6
                                    clip: true
                                    color: "transparent"
                                    Rectangle {
                                        width: 12
                                        height: 12
                                        radius: 6
                                        color: "transparent"
                                        border.width: 1.6
                                        border.color: Theme.alpha(Theme.fg, 0.8)
                                    }
                                }
                                RotationAnimation on rotation {
                                    running: tabItem.loading && root.visible
                                    from: 0
                                    to: 360
                                    duration: 800
                                    loops: Animation.Infinite
                                }
                            }
                            Image {
                                id: favicon
                                anchors.fill: parent
                                visible: !tabItem.loading && tabItem.icon !== "" && status === Image.Ready
                                source: tabItem.icon
                                sourceSize: Qt.size(30, 30)
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                            }
                            PathIcon {
                                anchors.fill: parent
                                visible: !tabItem.loading && !favicon.visible
                                name: "globe"
                                color: tabItem.ink
                                opacity: 0.6
                            }
                        }
                        Text {
                            id: tabTitle
                            anchors.left: tabIcon.right
                            anchors.leftMargin: 8
                            anchors.right: closeTab.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: tabItem.label
                            font.pixelSize: 13
                            color: tabItem.ink
                            elide: Text.ElideNone
                            clip: true
                            // mask-image: a cut title's last 14 px fade out
                            // into the tab's own background.
                            Rectangle {
                                visible: tabTitle.implicitWidth > tabTitle.width
                                anchors.right: parent.right
                                width: 14
                                height: parent.height
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0; color: Theme.alpha(Qt.tint(Theme.chatBg, tabItem.color), 0) }
                                    GradientStop { position: 1; color: Qt.tint(Theme.chatBg, tabItem.color) }
                                }
                            }
                        }
                        AbstractButton {
                            id: closeTab
                            objectName: "browserTabClose"
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            width: 20
                            height: 20
                            padding: 0
                            hoverEnabled: true
                            focusPolicy: Qt.NoFocus
                            Accessible.name: "Close tab"
                            opacity: tabHover.hovered || tabItem.active ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 150 } }
                            background: Rectangle {
                                radius: 6
                                color: closeTab.hovered ? Theme.alpha(Theme.fg, 0.1) : "transparent"
                            }
                            contentItem: Item {
                                PathIcon {
                                    anchors.centerIn: parent
                                    width: 14
                                    height: 14
                                    name: "browser-close"
                                    color: closeTab.hovered ? Theme.text : Theme.secondary
                                }
                            }
                            onClicked: root.browser.close(tabItem.handle)
                        }
                    }
                }
            }
        }
        BarButton {
            objectName: "browserNewTab"
            x: listBox.width + (tabList.count ? 4 : 0)
            text: "New tab"
            glyph: "browser-plus"
            onClicked: root.browser.newTab()
        }
    }

    // .browser-bar: back, forward, reload or stop, the address, open outside.
    Row {
        id: bar
        x: 8
        y: 42 + 3
        width: root.width - 16
        height: 33
        spacing: 2
        BarButton {
            objectName: "browserBack"
            anchors.verticalCenter: parent.verticalCenter
            text: "Back"
            glyph: "browser-back"
            enabled: root.browser.canGoBack
            onClicked: root.browser.back()
        }
        BarButton {
            objectName: "browserForward"
            anchors.verticalCenter: parent.verticalCenter
            text: "Forward"
            glyph: "browser-forward"
            enabled: root.browser.canGoForward
            onClicked: root.browser.forward()
        }
        BarButton {
            objectName: "browserReload"
            anchors.verticalCenter: parent.verticalCenter
            text: root.browser.loading ? "Stop loading" : "Reload"
            glyph: root.browser.loading ? "browser-stop" : "browser-reload"
            enabled: root.browser.ready
            onClicked: root.browser.reloadOrStop()
        }
        Item {
            width: bar.width - 4 * 30 - 4 * bar.spacing
            height: 33
            // .browser-address
            Rectangle {
                id: address
                objectName: "browserAddress"
                readonly property bool editing: field.activeFocus
                x: 4
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 8
                height: 32
                radius: 16
                color: Theme.alpha(Theme.fg, editing ? 0.08 : addressHover.hovered ? 0.07 : 0.05)
                border.width: 1
                border.color: Theme.alpha(Theme.fg, editing ? 0.22 : 0.05)
                Behavior on color { ColorAnimation { duration: 200 } }
                Behavior on border.color { ColorAnimation { duration: 200 } }
                HoverHandler { id: addressHover; cursorShape: Qt.IBeamCursor }
                TextInput {
                    id: field
                    objectName: "browserUrl"
                    anchors.fill: parent
                    leftPadding: 14
                    rightPadding: 14
                    verticalAlignment: TextInput.AlignVCenter
                    font.pixelSize: 13
                    color: address.editing ? Theme.text : "transparent"
                    selectionColor: Theme.selection
                    selectedTextColor: Theme.text
                    selectByMouse: true
                    clip: true
                    text: root.browser.url
                    Accessible.name: "Search or enter address"
                    onActiveFocusChanged: {
                        if (activeFocus)
                            selectAll()
                        else
                            text = root.browser.url
                    }
                    Keys.onReturnPressed: event => {
                        root.browser.go(text)
                        field.focus = false
                        root.forceActiveFocus()
                    }
                    Keys.onEnterPressed: event => {
                        root.browser.go(text)
                        field.focus = false
                        root.forceActiveFocus()
                    }
                    Keys.onEscapePressed: event => {
                        text = root.browser.url
                        field.focus = false
                        root.forceActiveFocus()
                    }
                    Connections {
                        target: root.browser
                        function onChanged() {
                            if (!field.activeFocus)
                                field.text = root.browser.url
                        }
                        // Next frame (requestAnimationFrame), once the panel is open.
                        function onFocusAddress() {
                            Qt.callLater(() => {
                                if (root.browser.open)
                                    field.forceActiveFocus()
                            })
                        }
                    }
                }
                Text {
                    anchors.fill: parent
                    leftPadding: 14
                    rightPadding: 14
                    verticalAlignment: Text.AlignVCenter
                    visible: field.text === ""
                    text: "Search or enter address"
                    font.pixelSize: 13
                    color: Theme.tertiary
                    elide: Text.ElideRight
                }
                // .browser-url-view: the address at rest, host first.
                Item {
                    objectName: "browserUrlView"
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    visible: !address.editing
                    clip: true
                    readonly property var parts: root.browser.urlParts(root.browser.url)
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        Text {
                            id: lead
                            text: parent.parent.parts.length === 3 ? parent.parent.parts[0] : ""
                            font.pixelSize: 13
                            color: Theme.tertiary
                        }
                        Text {
                            id: hostText
                            text: parent.parent.parts.length === 3 ? parent.parent.parts[1]
                                : parent.parent.parts.length === 1 ? parent.parent.parts[0] : ""
                            font.pixelSize: 13
                            color: parent.parent.parts.length === 3 ? Theme.text : Theme.secondary
                            width: Math.min(implicitWidth, parent.width - lead.width)
                            elide: Text.ElideRight
                        }
                        Text {
                            text: parent.parent.parts.length === 3 ? parent.parent.parts[2] : ""
                            font.pixelSize: 13
                            color: Theme.tertiary
                            width: Math.max(0, parent.width - lead.width - hostText.width)
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }
        BarButton {
            objectName: "browserExternal"
            anchors.verticalCenter: parent.verticalCenter
            text: "Open in your browser"
            glyph: "browser-external"
            onClicked: if (root.browser.url !== "") root.frontend.openExternal(root.browser.url)
        }
    }

    // .browser-stage: the pages and what covers them.
    Item {
        id: stage
        objectName: "browserStage"
        x: 6
        y: 84
        width: root.width - 12
        height: root.height - 84 - 6
        clip: true
        Rectangle {
            anchors.fill: parent
            color: Theme.alpha(Theme.fg, 0.02)
        }
        // One guest per tab with a view, kept while the tab lives; only the
        // active one shows, and none while closed unless the agent is using it.
        Repeater {
            model: root.browser.tabs
            delegate: Item {
                id: slot
                required property string handle
                required property bool active
                required property bool view
                required property int incarnation
                required property string source
                readonly property int live: view && source !== "" ? incarnation : 0
                property Item guest: null
                property int attached: 0
                anchors.fill: parent
                visible: active && (root.open || root.browser.agent)
                // After the event that changed it: a guest's own events (a
                // crash, a first load) must not replace that guest mid-signal.
                function attach() {
                    if (attached === live)
                        return
                    if (guest) {
                        guest.destroy()
                        guest = null
                    }
                    attached = live
                    if (live)
                        guest = guestComponent.createObject(slot, {
                            browser: root.browser, handle: handle, incarnation: live,
                            profile: root.siteProfile(), url: source })
                }
                onLiveChanged: Qt.callLater(attach)
                Component.onCompleted: Qt.callLater(attach)
            }
        }
        Component {
            id: guestComponent
            BrowserGuest {
                anchors.fill: parent
                enabled: !root.resizing
            }
        }
        // .browser-progress: a band sweeping the top edge while loading.
        Item {
            objectName: "browserProgress"
            z: 5
            width: parent.width
            height: 2
            clip: true
            opacity: root.browser.loading ? 0.9 : 0
            Behavior on opacity { NumberAnimation { duration: 300 } }
            Rectangle {
                id: band
                width: parent.width * 0.42
                height: 2
                radius: 2
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: Theme.alpha(Theme.accent, 0) }
                    GradientStop { position: 0.5; color: Theme.accent }
                    GradientStop { position: 1; color: Theme.alpha(Theme.accent, 0) }
                }
                NumberAnimation on x {
                    running: root.browser.loading && root.visible
                    from: -band.width
                    to: 2.4 * band.width
                    duration: 1100
                    loops: Animation.Infinite
                    easing.type: Easing.Bezier
                    easing.bezierCurve: Theme.motion
                }
            }
        }
        // .browser-empty: no page yet.
        Item {
            id: empty
            objectName: "browserEmpty"
            anchors.fill: parent
            visible: root.browser.blank
            opacity: 0
            onVisibleChanged: if (visible) emptyIn.restart()
            Component.onCompleted: if (visible) emptyIn.restart()
            ParallelAnimation {
                id: emptyIn
                NumberAnimation { target: empty; property: "opacity"; from: 0; to: 1; duration: Theme.reducedMotion ? 0 : 400; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
                NumberAnimation { target: emptyShift; property: "y"; from: 6; to: 0; duration: Theme.reducedMotion ? 0 : 400; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease }
            }
            transform: Translate { id: emptyShift }
            Column {
                anchors.centerIn: parent
                spacing: 14
                PathIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 40
                    height: 42
                    name: "ghost"
                    color: Theme.alpha(Theme.fg, 0.16)
                }
                Text {
                    width: Math.min(290, stage.width - 48)
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    lineHeightMode: Text.FixedHeight
                    lineHeight: 13.5 * 1.5
                    font.pointSize: Theme.points(13.5)
                    color: Theme.secondary
                    text: "Type an address above or ask OpenGhost to open a page. Sign in to your sites here, and OpenGhost can use them in any chat."
                }
            }
        }
        // .browser-error: the page could not be opened.
        Rectangle {
            objectName: "browserError"
            z: 2
            anchors.fill: parent
            visible: root.browser.error !== ""
            color: Theme.chatBg
            Column {
                anchors.centerIn: parent
                width: parent.width - 48
                spacing: 6
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: "This page couldn’t be opened"
                    font.pixelSize: 15
                    font.weight: 600
                    color: Theme.text
                    wrapMode: Text.Wrap
                }
                Text {
                    objectName: "browserErrorCode"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.browser.error
                    font.pixelSize: 13
                    color: Theme.secondary
                    wrapMode: Text.Wrap
                    bottomPadding: 10
                }
                Pill {
                    objectName: "browserRetry"
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Try again"
                    onClicked: root.browser.retry()
                }
            }
        }
        // .browser-toast: "Downloaded NAME" when a page's download finishes
        // (browser-panel.js notify), at the stage's bottom for 4.2 s.
        Rectangle {
            id: toast
            objectName: "browserToast"
            property string text
            property bool shown: false
            z: 5
            anchors.horizontalCenter: parent.horizontalCenter
            y: stage.height - 14 - height + (shown ? 0 : 8)
            width: Math.min(toastText.implicitWidth + 28, stage.width - 32)
            height: toastText.implicitHeight + 16
            radius: 14
            color: Theme.light ? Qt.rgba(1, 1, 1, 0.9) : Qt.rgba(22 / 255, 22 / 255, 22 / 255, 0.88)
            border.width: 1
            border.color: Theme.alpha(Theme.fg, 0.1)
            opacity: shown ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { enabled: !Theme.reducedMotion; NumberAnimation { duration: 300 } }
            Behavior on y { enabled: !Theme.reducedMotion; NumberAnimation { duration: 450; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
            Text {
                id: toastText
                objectName: "browserToastText"
                x: 14
                width: toast.width - 28
                anchors.verticalCenter: parent.verticalCenter
                text: toast.text
                elide: Text.ElideRight
                font.pointSize: Theme.points(12.5)
                color: Theme.text
            }
            Timer {
                id: toastTimer
                interval: 4200
                onTriggered: toast.shown = false
            }
            Connections {
                target: root.browser
                function onDownloaded(name) {
                    toast.text = "Downloaded " + name
                    toast.shown = true
                    toastTimer.restart()
                }
            }
        }
        // .browser-agent: the agent drives; the page takes no input meanwhile.
        Item {
            id: agentOverlay
            objectName: "browserAgent"
            z: 3
            anchors.fill: parent
            opacity: root.browser.driving ? 1 : 0
            visible: opacity > 0
            enabled: root.browser.driving
            Behavior on opacity { NumberAnimation { duration: 350 } }
            HoverHandler { id: agentHover }
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: wheel => wheel.accepted = true }
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                radius: 8
                border.width: 1.5
                border.color: Theme.alpha(Theme.accent, 0.55)
                SequentialAnimation on opacity {
                    running: agentOverlay.visible && !Theme.reducedMotion
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.45; duration: 1400; easing.type: Easing.InOutQuad }
                    NumberAnimation { to: 1; duration: 1400; easing.type: Easing.InOutQuad }
                }
            }
            Rectangle {
                objectName: "browserBadge"
                anchors.horizontalCenter: parent.horizontalCenter
                y: 12
                width: badgeRow.implicitWidth + 25
                height: 32
                radius: 16
                color: root.glass
                border.width: 1
                border.color: Theme.alpha(Theme.fg, 0.1)
                Row {
                    id: badgeRow
                    x: 11
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    PathIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 13
                        height: 14
                        name: "ghost"
                        color: Theme.accent
                        SequentialAnimation on anchors.verticalCenterOffset {
                            running: agentOverlay.visible && !Theme.reducedMotion
                            loops: Animation.Infinite
                            NumberAnimation { to: -1.5; duration: 1200; easing.type: Easing.InOutQuad }
                            NumberAnimation { to: 0; duration: 1200; easing.type: Easing.InOutQuad }
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "OpenGhost is using the browser"
                        font.pointSize: Theme.points(12.5)
                        font.weight: 550
                        color: Theme.text
                    }
                }
            }
            Pill {
                objectName: "browserTake"
                anchors.centerIn: parent
                size: 40
                fontSize: 14
                text: "Take control"
                opacity: agentHover.hovered || visualFocus ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 200 } }
                onClicked: root.browser.take()
            }
        }
        // .browser-user: the user has the browser until handing it back.
        Rectangle {
            objectName: "browserUser"
            z: 4
            anchors.horizontalCenter: parent.horizontalCenter
            y: root.browser.user ? 12 : 4
            visible: opacity > 0
            opacity: root.browser.user ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 250 } }
            Behavior on y { NumberAnimation { duration: 400; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
            width: userRow.implicitWidth + 18
            height: 32
            radius: 16
            color: root.glass
            border.width: 1
            border.color: Theme.alpha(Theme.fg, 0.1)
            Row {
                id: userRow
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "You’re in control"
                    font.pointSize: Theme.points(12.5)
                    font.weight: 550
                    color: Theme.text
                }
                Pill {
                    objectName: "browserHandBack"
                    anchors.verticalCenter: parent.verticalCenter
                    size: 24
                    fontSize: 12
                    text: "Hand back"
                    onClicked: root.browser.handBack()
                }
            }
        }
        // The stage's rounded corners (calc(--chat-radius - 4px)) and its
        // 1 px inset line, over the pages.
        Rectangle {
            z: 6
            anchors.fill: parent
            anchors.margins: -6
            radius: 14
            color: "transparent"
            border.width: 6
            border.color: Theme.chatBg
        }
        Rectangle {
            z: 6
            anchors.fill: parent
            radius: 8
            color: "transparent"
            border.width: 1
            border.color: Theme.alpha(Theme.fg, 0.05)
        }
    }
}
