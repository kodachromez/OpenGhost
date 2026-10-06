import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Shapes
import OpenGhost.Cpp

// OpenGhost's settings sheet. Runtime Plugins is capability-gated; all pages
// share the existing row/control styling, navigation and motion.
Popup {
    id: dialog
    objectName: "settingsDialog"
    required property var frontend
    readonly property var settings: frontend.settings
    readonly property var login: settings.login
    property string page: "general"
    readonly property var pages: [
        { id: "general", title: "General" },
        { id: "providers", title: "Providers" },
        ...(frontend.runtimePlugins ? [{ id: "plugins", title: "Plugins" }] : []),
        { id: "usage", title: "Usage" },
        { id: "appearance", title: "Appearance" }
    ]
    readonly property string title: pages.find(p => p.id === page)?.title ?? "General"
    // Capability loss/reconnect can remove the currently open page.
    onPagesChanged: Qt.callLater(function() {
        if (!dialog.pages.some(p => p.id === dialog.page))
            dialog.show("general", true)
        else if (dialog.visible)
            glide.place(true)
    })
    // Opening: opacity over 0.2 s (shown), rise 8 px and scale .98 over
    // 0.35 s (lift), the backdrop over 0.25 s (backdropShown). Closing retraces them.
    property real shown: 0
    property real lift: 0
    property real backdropShown: 0
    // OpenGhost's thinking levels as the presentation model names them (effort.*).
    readonly property var levelNames: ({
        off: "None", minimal: "Minimal", low: "Low", medium: "Medium",
        high: "High", xhigh: "XHigh", max: "Max"
    })

    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(780, (parent ? parent.width : 780) - 48)
    height: Math.min(640, (parent ? parent.height : 640) - 48)
    opacity: shown

    // Settings opens on its first page, General, its highlight already there.
    onAboutToShow: show("general", true)
    onOpened: {
        if (!settings.providersLoaded)
            frontend.refreshProviders()
        // A failed model discovery is asked again here, where its error shows.
        frontend.retryModels()
    }
    onAboutToHide: menu.close()
    Connections {
        target: dialog.settings
        function onModelWanted() {
            if (dialog.visible) {
                dialog.show("providers")
            }
        }
    }

    function show(name, instant) {
        const moved = name !== page
        page = name
        glide.place(instant || !moved)
        if (!moved)
            return
        flick.contentY = 0
        if (instant || Theme.reducedMotion) {
            bodyIn.stop()
            titleIn.stop()
            body.rise = title.rise = 1
            return
        }
        bodyIn.restart()
        titleIn.restart()
    }

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "shown"; from: 0; to: 1; duration: Theme.reducedMotion ? 0 : 200; easing.type: Easing.Bezier; easing.bezierCurve: [0.25, 0.1, 0.25, 1, 1, 1] }
            NumberAnimation { property: "lift"; from: 0; to: 1; duration: Theme.reducedMotion ? 0 : 350; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            NumberAnimation { property: "backdropShown"; from: 0; to: 1; duration: Theme.reducedMotion ? 0 : 250; easing.type: Easing.Bezier; easing.bezierCurve: [0.25, 0.1, 0.25, 1, 1, 1] }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "shown"; to: 0; duration: Theme.reducedMotion ? 0 : 200; easing.type: Easing.Bezier; easing.bezierCurve: [0.25, 0.1, 0.25, 1, 1, 1] }
            NumberAnimation { property: "lift"; to: 0; duration: Theme.reducedMotion ? 0 : 350; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            NumberAnimation { property: "backdropShown"; to: 0; duration: Theme.reducedMotion ? 0 : 250; easing.type: Easing.Bezier; easing.bezierCurve: [0.25, 0.1, 0.25, 1, 1, 1] }
        }
    }
    // The backdrop (--backdrop) fades in 0.25 s. The popup's own opacity
    // also reaches it, so its fade is divided out.
    Overlay.modal: Rectangle {
        color: Theme.alpha(Theme.backdrop, dialog.shown > 0.001 ? Math.min(1, dialog.backdropShown / dialog.shown) : 0)
    }

    background: null
    contentItem: Item {
        id: sheet
        transform: Translate { y: 8 * (1 - dialog.lift) }
        scale: 0.98 + 0.02 * dialog.lift

        // 0 24px 64px rgba(0, 0, 0, .45 × --shadow), the double contour and
        // the window's backdrop (var(--frame), var(--app-bg), laid out in the
        // sheet), radius 20 (--chat-radius + --chat-gap). Light has no inset
        // line: a 1 px rgba(30, 48, 88, .18) outline instead.
        BoxShadow {
            anchors.fill: parent
            radius: 20
            blur: 64
            offsetY: 24
            color: Theme.alpha("black", 0.45 * Theme.shadow)
        }
        Rectangle {
            readonly property real line: Theme.light ? 1 : 1.5
            anchors.fill: parent
            anchors.margins: -line
            radius: 20 + line
            antialiasing: true
            color: Theme.light ? Qt.rgba(30 / 255, 48 / 255, 88 / 255, 0.18) : Theme.contourOuter
        }
        Rectangle {
            anchors.fill: parent
            radius: 20
            antialiasing: true
            visible: !Theme.light
            color: Theme.contourInner
        }
        Backdrop {
            readonly property real inset: Theme.light ? 0 : 1.5
            anchors.fill: parent
            anchors.margins: inset
            radius: 20 - inset
            box: Qt.size(parent.width, parent.height)
            origin: Qt.point(inset, inset)
        }
        // A press anywhere on the sheet stays in it.
        MouseArea {
            anchors.fill: parent
        }

        // .settings-nav: 180 px, padding 18/10: the heading, then the tabs.
        Item {
            id: nav
            width: 180
            height: parent.height
            CssText {
                x: 22
                y: 18 + (32 - line) / 2
                text: "Settings"
                size: 15
                cssWeight: 600
                font.letterSpacing: -0.15
                color: Theme.text
            }
            Item {
                id: tabs
                objectName: "settingsTabs"
                x: 10
                y: 60
                width: 160
                height: tabColumn.height
                // One highlight for all sections: it glides to the chosen one
                // (0.5 s, the motion easing) and fades in once (0.2 s).
                Rectangle {
                    id: glide
                    objectName: "settingsGlide"
                    property bool placed: false
                    width: parent.width
                    height: 34
                    radius: 17
                    // The chosen tab is a small card, as the chosen chat is.
                    color: Theme.rowActive
                    RowLift { radius: 17 }
                    opacity: placed ? 1 : 0
                    Behavior on opacity {
                        enabled: !Theme.reducedMotion
                        NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: [0.25, 0.1, 0.25, 1, 1, 1] }
                    }
                    Behavior on y {
                        id: glideMotion
                        NumberAnimation { duration: 500; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                    }
                    function place(instant) {
                        const at = dialog.pages.findIndex(p => p.id === dialog.page)
                        glideMotion.enabled = !instant && !Theme.reducedMotion
                        y = at * 38
                        glideMotion.enabled = true
                        placed = true
                    }
                }
                Column {
                    id: tabColumn
                    width: parent.width
                    spacing: 4
                    Repeater {
                        model: dialog.pages
                        delegate: SettingsTab {}
                    }
                }
                function step(by) {
                    const at = dialog.pages.findIndex(p => p.id === dialog.page)
                    const next = (at + by + dialog.pages.length) % dialog.pages.length
                    // Focus first: the tab left behind stops taking Tab focus.
                    tabColumn.children[next].forceActiveFocus(Qt.TabFocusReason)
                    dialog.show(dialog.pages[next].id)
                }
            }
        }

        // .settings-page: the inset panel, 8 px from the sheet's top, right
        // and bottom edges, on the chat background with the double contour.
        Rectangle {
            x: 180 - 1.5
            y: 8 - 1.5
            width: parent.width - 188 + 3
            height: parent.height - 16 + 3
            radius: 13.5
            antialiasing: true
            color: Theme.contourOuter
            Rectangle {
                anchors.fill: parent
                anchors.margins: 1.5
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
        }
        Flickable {
            id: flick
            objectName: "settingsPage"
            x: 180
            y: 8
            width: parent.width - 188
            height: parent.height - 16
            clip: true
            contentWidth: width
            // Padding 10 px (--toggle-inset) on top, 24 px at the sides and bottom.
            contentHeight: 10 + pageColumn.height + 24
            boundsBehavior: Flickable.StopAtBounds
            Column {
                id: pageColumn
                x: 24
                y: 10
                width: flick.width - 48
                // .settings-page-header: 32 px, 14 px into the right padding,
                // 8 px above the page.
                Item {
                    width: parent.width + 14
                    height: 40
                    CssText {
                        id: title
                        objectName: "settingsTitle"
                        property real rise: 1
                        // Centred in the header; Chromium rounds its ascent
                        // and draws it a pixel higher than Qt (measured).
                        y: (32 - line) / 2 - 1 + 3 * (1 - rise)
                        text: dialog.title
                        size: 17
                        cssWeight: 600
                        color: Theme.text
                        opacity: rise
                        layer.enabled: rise < 1
                        layer.effect: MultiEffect {
                            blurEnabled: true
                            blurMax: 16
                            autoPaddingEnabled: true
                            // blur(4px) on the way in.
                            blur: Math.min(1, 4 * (1 - title.rise) / (0.27 * 16))
                        }
                        NumberAnimation on rise {
                            id: titleIn
                            running: false
                            from: 0
                            to: 1
                            duration: 320
                            easing.type: Easing.Bezier
                            easing.bezierCurve: Theme.motion
                        }
                    }
                    CloseButton {
                        objectName: "settingsClose"
                        x: parent.width - width
                        onClicked: dialog.close()
                    }
                }
                Item {
                    id: body
                    property real rise: 1
                    width: parent.width
                    height: pageStack.height
                    opacity: rise
                    transform: Translate { y: 10 * (1 - body.rise) }
                    layer.enabled: rise < 1
                    layer.effect: MultiEffect {
                        blurEnabled: true
                        blurMax: 24
                        autoPaddingEnabled: true
                        // blur(6px) on the way in.
                        blur: Math.min(1, 6 * (1 - body.rise) / (0.27 * 24))
                    }
                    NumberAnimation on rise {
                        id: bodyIn
                        running: false
                        from: 0
                        to: 1
                        duration: 460
                        easing.type: Easing.Bezier
                        easing.bezierCurve: Theme.motion
                    }
                    Item {
                        id: pageStack
                        width: parent.width
                        height: dialog.page === "general" ? generalPage.height
                              : dialog.page === "providers" ? providersPage.height
                              : dialog.page === "plugins" ? pluginsPage.height
                              : dialog.page === "usage" ? usagePage.height
                              : appearancePage.height
                        GeneralPage {
                            id: generalPage
                            objectName: "generalPage"
                            width: parent.width
                            store: dialog.frontend.general
                            visible: dialog.page === "general"
                        }
                        Column {
                            id: providersPage
                            width: parent.width
                            visible: dialog.page === "providers"
                            SettingsRow {
                                first: true
                                visible: !dialog.settings.providersLoaded || !!dialog.settings.providersError
                                label: "Providers"
                                hint: dialog.settings.providersError ? "Backend not connected" : dialog.settings.providersLoaded ? "" : "Loading providers…"
                                Status {
                                    text: dialog.settings.providersError
                                    error: true
                                }
                            }
                            Repeater {
                                model: dialog.settings.providersLoaded ? dialog.settings.providers : []
                                delegate: ProviderRow {}
                            }
                        }
                        Column {
                            id: pluginsPage
                            objectName: "pluginsPage"
                            width: parent.width
                            visible: dialog.frontend.runtimePlugins && dialog.page === "plugins"
                            SettingsRow {
                                first: true
                                visible: dialog.frontend.pluginsLoading || !dialog.frontend.pluginsLoaded
                                         || !!dialog.frontend.pluginsError || !dialog.frontend.pluginCount
                                label: "Plugins"
                                hint: dialog.frontend.pluginsLoading ? "Loading plugins…"
                                    : !dialog.frontend.pluginsLoaded ? "Plugin state unavailable"
                                    : !dialog.frontend.pluginCount ? "No plugins reported by the backend" : ""
                                PillButton {
                                    objectName: "pluginsRefresh"
                                    visible: !!dialog.frontend.pluginsError
                                    enabled: !dialog.frontend.pluginsLoading
                                    text: "Refresh"
                                    onClicked: dialog.frontend.refreshPlugins()
                                }
                            }
                            Status {
                                objectName: "pluginsError"
                                width: parent.width
                                visible: !!text
                                text: dialog.frontend.pluginsError
                                error: true
                            }
                            // A keyed model updated in place: a pending flip or an
                            // event changes roles, not delegates, so focus stays.
                            Repeater {
                                model: dialog.frontend.runtimePlugins ? dialog.frontend.plugins : null
                                delegate: Column {
                                    id: plugin
                                    required property int index
                                    required property string pluginId
                                    required property string name
                                    required property string description
                                    required property string status
                                    required property bool enabled
                                    required property bool available
                                    required property bool canToggle
                                    required property string note
                                    required property bool noteError
                                    objectName: "plugin-" + pluginId
                                    width: pluginsPage.width
                                    SettingsRow {
                                        first: plugin.index === 0
                                        label: plugin.name
                                        hint: [plugin.pluginId !== plugin.name ? plugin.pluginId : "",
                                               plugin.description, plugin.status].filter(Boolean).join("\n")
                                        PillButton {
                                            objectName: "pluginToggle-" + plugin.pluginId
                                            // Stays focusable while pending/unconfirmed (a disabled
                                            // item drops keyboard focus); only an available row
                                            // in a toggleable state acts. Not checkable: a press
                                            // cannot overwrite the backend's state.
                                            enabled: plugin.available
                                            actionable: plugin.canToggle
                                            text: plugin.enabled ? "On" : "Off"
                                            Accessible.name: plugin.name + (plugin.enabled ? ": turn off" : ": turn on")
                                            Accessible.description: plugin.status
                                            onClicked: if (plugin.canToggle)
                                                dialog.frontend.setPluginEnabled(plugin.pluginId, !plugin.enabled)
                                        }
                                    }
                                    Status {
                                        objectName: "pluginNote-" + plugin.pluginId
                                        width: parent.width
                                        visible: !!text
                                        text: plugin.note
                                        error: plugin.noteError
                                    }
                                }
                            }
                        }
                        UsagePage {
                            id: usagePage
                            width: parent.width
                            frontend: dialog.frontend
                            active: dialog.opened && dialog.page === "usage"
                            visible: dialog.page === "usage"
                        }
                        AppearancePage {
                            id: appearancePage
                            width: parent.width
                            visible: dialog.page === "appearance"
                        }
                    }
                }
            }
        }
        // As in the chat, the page fades into its top and bottom edges while
        // more of it lies scrolled out past them (0.3 s).
        Rectangle {
            x: 180
            y: 8
            width: flick.width
            height: 28
            topLeftRadius: 12
            topRightRadius: 12
            opacity: flick.contentY > 1 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.reducedMotion ? 0 : 300 } }
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.chatBg }
                GradientStop { position: 0.7; color: Theme.alpha(Theme.chatBg, 0) }
            }
        }
        Rectangle {
            x: 180
            y: parent.height - 8 - height
            width: flick.width
            height: 28
            bottomLeftRadius: 12
            bottomRightRadius: 12
            opacity: flick.contentY + flick.height < flick.contentHeight - 1 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.reducedMotion ? 0 : 300 } }
            // linear-gradient(transparent, var(--chat-bg) 70%).
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.alpha(Theme.chatBg, 0) }
                GradientStop { position: 0.7; color: Theme.chatBg }
            }
        }
        // The app's thin scrollbar (scrollbar.js): 12 px to grab, a 4 px
        // thumb (6 px under the pointer), 2 px inside its track.
        Item {
            id: bar
            objectName: "settingsScrollbar"
            readonly property real room: height - 4
            readonly property bool scrollable: flick.contentHeight - flick.height > 1
            readonly property real thumb: Math.max(24, room * flick.height / Math.max(1, flick.contentHeight))
            x: parent.width - 10 - width
            y: 20
            width: 12
            height: parent.height - 40
            opacity: scrollable ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.reducedMotion ? 0 : 200 } }
            Rectangle {
                readonly property bool lit: barArea.containsMouse || barArea.pressed
                anchors.horizontalCenter: parent.horizontalCenter
                y: 2 + (bar.room - bar.thumb) * Math.max(0, Math.min(1, flick.contentY / Math.max(1, flick.contentHeight - flick.height)))
                width: lit ? 6 : 4
                height: bar.thumb
                radius: 3
                color: lit ? Theme.secondary : Theme.tertiary
                Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
                Behavior on color { ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
            }
            MouseArea {
                id: barArea
                property real grab: 0
                anchors.fill: parent
                hoverEnabled: true
                onPressed: mouse => grab = mouse.y
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const span = bar.room - bar.thumb
                    if (span > 0)
                        flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height,
                            flick.contentY + (mouse.y - grab) / span * (flick.contentHeight - flick.height)))
                    grab = mouse.y
                }
            }
        }
    }

    // CSS text in a line box of `line` px, its glyphs where Chromium puts
    // them: half the leading below the box's top.
    component CssText: Text {
        property real size: 15
        property real line: size * 1.5
        property int cssWeight: 400
        font.pointSize: Theme.points(size)
        font.weight: Theme.weight(cssWeight)
        lineHeightMode: Text.FixedHeight
        lineHeight: line
        topPadding: Theme.halfLeading(font, line)
        bottomPadding: -Theme.halfLeading(font, line)
        // Whole CSS line boxes (Qt would round 22.5 px lines down).
        height: topPadding - Theme.halfLeading(font, line) + Math.max(1, lineCount) * line
        textFormat: Text.PlainText
    }

    // .settings-tab: 34 px, radius 17, its icon and name in secondary,
    // primary when hovered or chosen. The icon moves as it is chosen.
    component SettingsTab: AbstractButton {
        id: tab
        required property var modelData
        required property int index
        readonly property bool current: dialog.page === modelData.id
        readonly property color ink: current || hovered ? Theme.text : Theme.secondary
        objectName: "settingsTab-" + modelData.id
        width: 160
        height: 34
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        activeFocusOnTab: current
        Accessible.role: Accessible.PageTab
        text: modelData.title
        onClicked: dialog.show(modelData.id)
        Keys.onUpPressed: tabs.step(-1)
        Keys.onDownPressed: tabs.step(1)
        // :focus-visible: a 2 px ring just outside.
        background: Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: 19
            color: "transparent"
            visible: tab.visualFocus
            border.width: 2
            border.color: Theme.alpha(Theme.fg, 0.35)
        }
        contentItem: Item {
            TabIcon {
                x: 10
                y: 8.5
                page: tab.modelData.id
                chosen: tab.current
                color: tab.ink
            }
            CssText {
                x: 37
                y: (34 - line) / 2
                text: tab.modelData.title
                size: 14
                cssWeight: 500
                color: tab.ink
                Behavior on color { ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
            }
        }
    }

    // The section icons (viewBox 30 30 60 60, stroke 5.5, 17 px): General's
    // sliders slide, the plug pushes in, the bars trade heights and the half
    // moon turns over (0.6 s, the moon 0.7 s, the motion easing).
    component TabIcon: Item {
        id: icon
        property string page
        property bool chosen
        property color color
        property real t: chosen ? 1 : 0
        // The plug's push: up 5 units at 40 % of 0.6 s, back by the end.
        property real push: 0
        width: 17
        height: 17
        Behavior on t {
            enabled: !Theme.reducedMotion
            NumberAnimation { duration: icon.page === "appearance" ? 700 : 600; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
        }
        onChosenChanged: {
            if (chosen && (page === "providers" || page === "plugins") && !Theme.reducedMotion)
                plug.restart()
        }
        SequentialAnimation {
            id: plug
            NumberAnimation { target: icon; property: "push"; from: 0; to: 1; duration: 240; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            NumberAnimation { target: icon; property: "push"; to: 0; duration: 360; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
        }
        Shape {
            width: 60
            height: 60
            scale: 17 / 60
            transformOrigin: Item.TopLeft
            preferredRendererType: Shape.CurveRenderer
            // Sliders: two tracks, each cut by a knob that slides 12 units.
            ShapePath {
                strokeColor: icon.page === "general" ? icon.color : "transparent"
                strokeWidth: 5.5
                capStyle: ShapePath.RoundCap
                fillColor: "transparent"
                PathMove { x: 8; y: 17 }
                PathLine { x: 16 + 12 * icon.t; y: 17 }
                PathMove { x: 28 + 12 * icon.t; y: 17 }
                PathLine { x: 52; y: 17 }
                PathMove { x: 8; y: 43 }
                PathLine { x: 32 - 12 * icon.t; y: 43 }
                PathMove { x: 44 - 12 * icon.t; y: 43 }
                PathLine { x: 52; y: 43 }
            }
            ShapePath {
                strokeColor: icon.page === "general" ? icon.color : "transparent"
                strokeWidth: 5.5
                fillColor: "transparent"
                PathAngleArc { centerX: 22 + 12 * icon.t; centerY: 17; radiusX: 6; radiusY: 6; startAngle: 0; sweepAngle: 360 }
                PathAngleArc { centerX: 38 - 12 * icon.t; centerY: 43; radiusX: 6; radiusY: 6; startAngle: 0; sweepAngle: 360; moveToStart: true }
            }
            // The plug: its prongs and body (pushed in), then the cord.
            ShapePath {
                strokeColor: icon.page === "providers" || icon.page === "plugins" ? icon.color : "transparent"
                strokeWidth: 5.5
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                fillColor: "transparent"
                PathMove { x: 22; y: 6 - 5 * icon.push }
                PathLine { x: 22; y: 16 - 5 * icon.push }
                PathMove { x: 38; y: 6 - 5 * icon.push }
                PathLine { x: 38; y: 16 - 5 * icon.push }
                PathMove { x: 14; y: 16 - 5 * icon.push }
                PathLine { x: 46; y: 16 - 5 * icon.push }
                PathLine { x: 46; y: 23 - 5 * icon.push }
                PathArc { x: 31; y: 38 - 5 * icon.push; radiusX: 15; radiusY: 15 }
                PathLine { x: 29; y: 38 - 5 * icon.push }
                PathArc { x: 14; y: 23 - 5 * icon.push; radiusX: 15; radiusY: 15 }
                PathLine { x: 14; y: 16 - 5 * icon.push }
            }
            ShapePath {
                strokeColor: icon.page === "providers" || icon.page === "plugins" ? icon.color : "transparent"
                strokeWidth: 5.5
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                fillColor: "transparent"
                PathMove { x: 30; y: 38 }
                PathLine { x: 30; y: 46 }
                PathArc { x: 24; y: 52; radiusX: 6; radiusY: 6 }
                PathLine { x: 20; y: 52 }
            }
            // Bars at 42, 60 and 78, trading heights.
            ShapePath {
                strokeColor: icon.page === "usage" ? icon.color : "transparent"
                strokeWidth: 5.5
                capStyle: ShapePath.RoundCap
                fillColor: "transparent"
                PathMove { x: 12; y: 50 }
                PathLine { x: 12; y: 32 - 14 * icon.t }
                PathMove { x: 30; y: 50 }
                PathLine { x: 30; y: 10 + 20 * icon.t }
                PathMove { x: 48; y: 50 }
                PathLine { x: 48; y: 22 - 14 * icon.t }
            }
            // Contrast: the circle; its filled half turns over below.
            ShapePath {
                strokeColor: icon.page === "appearance" ? icon.color : "transparent"
                strokeWidth: 5.5
                fillColor: "transparent"
                PathAngleArc { centerX: 30; centerY: 30; radiusX: 21; radiusY: 21; startAngle: 0; sweepAngle: 360 }
            }
        }
        Item {
            width: 17
            height: 17
            visible: icon.page === "appearance"
            rotation: 180 * icon.t
            Shape {
            width: 60
            height: 60
            scale: 17 / 60
            transformOrigin: Item.TopLeft
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: icon.color
                strokeWidth: 5.5
                joinStyle: ShapePath.RoundJoin
                fillColor: icon.color
                PathMove { x: 30; y: 9 }
                PathArc { x: 30; y: 51; radiusX: 21; radiusY: 21 }
                PathLine { x: 30; y: 9 }
            }
            }
        }
    }

    // close-button.js: 32 px, round, a 22 px cross that turns 90° under the
    // pointer (spring 170/22) and sinks and fades while pressed (230/27).
    component CloseButton: AbstractButton {
        id: close
        width: 32
        height: 32
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: "Close"
        Spring { id: turn; goal: close.hovered ? 1 : 0; k: 170; c: 22 }
        Spring { id: press; goal: close.down ? 1 : 0; k: 230; c: 27 }
        background: Rectangle {
            radius: 16
            color: "transparent"
            border.width: close.visualFocus ? 2 : 0
            border.color: Theme.alpha(Theme.fg, 0.35)
        }
        contentItem: Item {
            opacity: close.hovered || close.visualFocus ? 0.85 : 0.55
            Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
            PathIcon {
                anchors.centerIn: parent
                anchors.verticalCenterOffset: press.value * 2 * 22 / 60
                width: 22
                height: 22
                name: "cross"
                color: Theme.strong
                rotation: 90 * turn.value
                opacity: 1 - 0.3 * press.value
            }
        }
    }

    // .message-action: a 32 px pill on the composer colour, the control
    // hover fill under the pointer and the 2 px focus ring.
    component PillButton: AbstractButton {
        id: pill
        // false: shown and focusable like a disabled button, but its owner ignores clicks.
        property bool actionable: true
        height: 32
        implicitWidth: label.implicitWidth + 28
        width: implicitWidth
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        opacity: enabled && actionable ? 1 : 0.5
        background: Item {
            Rectangle {
                anchors.fill: parent
                anchors.margins: -2
                radius: 18
                visible: pill.visualFocus
                color: Theme.alpha(Theme.fg, 0.35)
            }
            Rectangle {
                anchors.fill: parent
                radius: 16
                antialiasing: true
                color: Theme.composerBorder
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: 15
                    antialiasing: true
                    color: pill.hovered && pill.actionable ? Theme.hover : Theme.composerBg
                    Behavior on color { ColorAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.ease } }
                }
            }
        }
        contentItem: Item {
            CssText {
                id: label
                x: 14
                y: (32 - line) / 2
                text: pill.text
                size: 14
                color: Theme.text
            }
        }
    }

    // .settings-status: 13 px/18 px, at least one line, 6 px below the
    // controls and 16 px in from their right; the danger colour for errors.
    component Status: Text {
        property bool error: false
        width: Math.min(implicitWidth, 320)
        font.pointSize: Theme.points(13)
        lineHeightMode: Text.FixedHeight
        lineHeight: 18
        topPadding: 6 + Theme.halfLeading(font, 18)
        bottomPadding: -Theme.halfLeading(font, 18)
        rightPadding: 16
        height: 6 + Math.max(1, lineCount) * 18
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignRight
        textFormat: Text.PlainText
        color: error ? Theme.danger : Theme.secondary
    }

    // .settings-row: the label and hint, the control on the right, 18 px
    // above and below, and a .06 line (a 1 px border) above every row but
    // the first.
    component SettingsRow: Item {
        id: row
        property bool first: false
        property string label
        property string hint
        default property alias control: controls.data
        readonly property real rule: first ? 0 : 1
        width: parent ? parent.width : 0
        height: visible ? rule + Math.max(texts.height, controls.height) + 36 : 0
        Rectangle {
            width: parent.width
            height: 1
            visible: !row.first
            color: Theme.alpha(Theme.fg, 0.06)
        }
        Column {
            id: texts
            y: row.rule + 18
            width: row.width - controls.width - 24
            CssText {
                width: parent.width
                text: row.label
                size: 15
                cssWeight: 500
                color: Theme.text
                wrapMode: Text.Wrap
            }
            CssText {
                width: parent.width
                visible: text.length > 0
                topPadding: 4 + Theme.halfLeading(font, line)
                text: row.hint
                size: 13
                line: 18
                color: Theme.secondary
                wrapMode: Text.Wrap
            }
        }
        Item {
            id: controls
            x: row.width - width
            y: row.rule + 18
            width: childrenRect.width
            height: childrenRect.height
        }
    }

    // A provider's row (OpenGhost's settings.js providerRow): its name, how it
    // signs in and whether it is connected; Log out, Sign in or Add API key,
    // or the login step's controls, then its note or the step's progress.
    component ProviderRow: SettingsRow {
        id: provider
        required property var modelData
        required property int index
        readonly property bool signingIn: dialog.login.providerId === modelData.id
        readonly property var step: dialog.login
        objectName: "provider-" + modelData.id
        first: index === 0
        label: modelData.name
        hint: modelData.hint
        Column {
            Row {
                anchors.right: parent.right
                spacing: 8
                PillButton {
                    objectName: "logout"
                    visible: provider.modelData.logout && !provider.signingIn
                    enabled: !dialog.login.id
                    text: "Log out"
                    onClicked: dialog.frontend.logout(provider.modelData.id)
                }
                PillButton {
                    objectName: "oauth"
                    visible: provider.modelData.oauth && !provider.signingIn
                    enabled: !dialog.login.id
                    text: "Sign in"
                    onClicked: dialog.frontend.login(provider.modelData.id, "oauth")
                }
                PillButton {
                    objectName: "apiKey"
                    visible: provider.modelData.apiKey && !provider.signingIn
                    enabled: !dialog.login.id
                    text: "Add API key"
                    onClicked: dialog.frontend.login(provider.modelData.id, "api_key")
                }
                Repeater {
                    model: provider.signingIn ? provider.step.links || [] : []
                    delegate: PillButton {
                        required property var modelData
                        text: modelData.label || "Open link"
                        onClicked: dialog.frontend.openExternal(modelData.url)
                    }
                }
                PillButton {
                    visible: provider.signingIn && !!provider.step.url
                    text: provider.step.type === "device_code" ? "Open verification page"
                                                               : "Open sign-in page"
                    onClicked: dialog.frontend.openExternal(provider.step.url)
                }
                PillButton {
                    visible: provider.signingIn && !!provider.step.userCode
                    text: "Copy code"
                    onClicked: dialog.frontend.copy(provider.step.userCode)
                }
                PillButton {
                    id: option
                    objectName: "loginOption"
                    property int currentIndex: 0
                    readonly property var options: provider.signingIn ? provider.step.options || [] : []
                    readonly property string currentValue: options[currentIndex]?.id ?? ""
                    readonly property string prompt: provider.signingIn ? provider.step.promptId || "" : ""
                    onPromptChanged: currentIndex = 0
                    visible: provider.signingIn && provider.step.type === "select"
                    text: options[currentIndex]?.label ?? ""
                    onClicked: menu.openFor("login", option)
                }
                PillButton {
                    objectName: "loginContinue"
                    visible: provider.signingIn && !!provider.step.promptId
                    text: provider.step.type === "select" ? "Choose" : "Continue"
                    onClicked: provider.step.type === "select"
                               ? dialog.frontend.answerLogin(provider.step.id, provider.step.promptId, option.currentValue)
                               : answer.send()
                }
                PillButton {
                    objectName: "loginCancel"
                    visible: provider.signingIn
                    text: "Cancel"
                    onClicked: dialog.frontend.cancelLogin(provider.step.id)
                }
            }
            // .settings-key: the step's answer, 260 × 36, 8 px below.
            Item {
                anchors.right: parent.right
                width: 260
                height: answer.visible ? 44 : 0
                TextField {
                    id: answer
                    objectName: "loginAnswer"
                    readonly property string prompt: provider.signingIn ? provider.step.promptId || "" : ""
                    y: 8
                    width: 260
                    height: 36
                    visible: provider.signingIn && !!provider.step.input
                    leftPadding: 16
                    rightPadding: 42
                    topPadding: 0
                    bottomPadding: 0
                    verticalAlignment: TextInput.AlignVCenter
                    font.family: Theme.mono
                    font.pointSize: Theme.points(13)
                    color: Theme.text
                    placeholderText: provider.step.placeholder || provider.step.inputLabel || ""
                    placeholderTextColor: Theme.muted
                    selectionColor: Theme.selection
                    selectedTextColor: Theme.text
                    echoMode: provider.step.secret ? TextInput.Password : TextInput.Normal
                    inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                                      | (provider.step.secret ? Qt.ImhSensitiveData | Qt.ImhHiddenText : 0)
                    background: Rectangle {
                        radius: 18
                        color: Theme.composerBg
                        border.width: 1
                        border.color: answer.activeFocus ? Theme.alpha(Theme.fg, 0.2) : Theme.composerBorder
                    }
                    onPromptChanged: clear()
                    onVisibleChanged: if (visible) forceActiveFocus()
                    Keys.onReturnPressed: send()
                    Keys.onEnterPressed: send()
                    function send() {
                        const value = text
                        clear()
                        dialog.frontend.answerLogin(provider.step.id, prompt, value)
                    }
                }
            }
            Status {
                objectName: "providerNote"
                anchors.right: parent.right
                visible: !provider.signingIn
                text: provider.modelData.note
                error: provider.modelData.error
            }
            Status {
                objectName: "loginMessage"
                anchors.right: parent.right
                visible: provider.signingIn
                text: provider.step.message || ""
            }
        }
    }

    // The choice menu (.mode-menu): 316 px on the composer colour, radius 18,
    // below its button and flush with its right edge (above it when there is
    // no room), rising in (0.18 s fade, 0.4 s from translateY(6) scale .96).
    Popup {
        id: menu
        objectName: "settingsMenu"
        property string kind
        property Item anchor
        property var items: []
        property real rise: 0
        function openFor(what, button) {
            const list = entries(what, button)
            if (!list.length || !button || !button.enabled)
                return
            kind = what
            anchor = button
            items = list
            const at = button.mapToItem(dialog.contentItem, button.width, button.height)
            const tall = Math.min(320, 12 + list.length * 54)
            x = at.x - width
            y = at.y + 6 + tall > dialog.height && at.y - button.height - 6 - tall >= 0
                ? at.y - button.height - 6 - tall : at.y + 6
            open()
        }
        function entries(what, button) {
            const s = dialog.settings
            if (what === "provider")
                return s.providerIds.map((id, i) => ({
                    value: id, title: s.providerNames[i], glyph: "globe",
                    hint: s.choices.filter(c => c.provider === id).length + " available",
                    checked: id === s.provider
                }))
            if (what === "model")
                return s.modelIds.map((id, i) => ({
                    value: id, title: s.modelNames[i], hint: id, glyph: "ghost",
                    checked: !s.invalid && id === s.model
                }))
            if (what === "thinking")
                return s.levels.map(level => ({
                    value: level, title: dialog.levelNames[level] ?? level, hint: "", glyph: "bubble",
                    checked: level === s.thinking
                }))
            if (what === "login" && button)
                return (button.options || []).map((o, i) => ({
                    value: i, title: o.label, hint: o.description || "", glyph: "globe",
                    checked: i === button.currentIndex
                }))
            return []
        }
        function pick(value) {
            const s = dialog.settings
            close()
            if (kind === "provider")
                s.chooseProvider(value)
            else if (kind === "model")
                s.chooseModel(value)
            else if (kind === "thinking")
                s.chooseThinking(value)
            else if (kind === "login" && anchor)
                anchor.currentIndex = value
        }
        width: 316
        height: Math.min(320, list.contentHeight + 12)
        padding: 6
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        // Focus goes to the chosen option and the highlight with it; the
        // focus ring shows only once the keyboard moves it (:focus-visible).
        onAboutToShow: {
            list.keyed = !!anchor && anchor.visualFocus
            list.currentIndex = Math.max(0, items.findIndex(item => item.checked))
            list.hovered = list.currentIndex
        }
        onOpened: list.forceActiveFocus()
        onClosed: if (anchor && dialog.visible) anchor.forceActiveFocus()
        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.reducedMotion ? 0 : 180; easing.type: Easing.Bezier; easing.bezierCurve: [0.25, 0.1, 0.25, 1, 1, 1] }
                NumberAnimation { property: "rise"; from: 0; to: 1; duration: Theme.reducedMotion ? 0 : 400; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
        }
        exit: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; to: 0; duration: Theme.reducedMotion ? 0 : 180; easing.type: Easing.Bezier; easing.bezierCurve: [0.25, 0.1, 0.25, 1, 1, 1] }
                NumberAnimation { property: "rise"; to: 0; duration: Theme.reducedMotion ? 0 : 400; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
            }
        }
        // transform-origin: its top right corner.
        background: Item {
            transform: [
                Scale { origin.x: 316; origin.y: 0; xScale: 0.96 + 0.04 * menu.rise; yScale: 0.96 + 0.04 * menu.rise },
                Translate { y: 6 * (1 - menu.rise) }
            ]
            BoxShadow {
                anchors.fill: parent
                radius: 18
                blur: 48
                offsetY: 18
                color: Theme.alpha("black", 0.5 * Theme.shadow)
            }
            Rectangle {
                anchors.fill: parent
                radius: 18
                antialiasing: true
                color: Qt.tint(Theme.composerBg, Theme.alpha(Theme.fg, 0.08))
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: 17
                    antialiasing: true
                    color: Theme.composerBg
                }
            }
        }
        contentItem: ListView {
            id: list
            property int hovered: -1
            property bool keyed: false
            onCurrentIndexChanged: if (activeFocus) hovered = currentIndex
            Keys.onUpPressed: event => { keyed = true; event.accepted = false }
            Keys.onDownPressed: event => { keyed = true; event.accepted = false }
            transform: [
                Scale { origin.x: 310; origin.y: -6; xScale: 0.96 + 0.04 * menu.rise; yScale: 0.96 + 0.04 * menu.rise },
                Translate { y: 6 * (1 - menu.rise) }
            ]
            clip: true
            implicitHeight: contentHeight
            model: menu.items
            boundsBehavior: Flickable.StopAtBounds
            keyNavigationWraps: true
            highlightFollowsCurrentItem: false
            // .mode-glide: one highlight under the hovered option (0.32 s).
            Rectangle {
                id: menuGlide
                parent: list.contentItem
                readonly property Item under: list.hovered >= 0 ? list.itemAtIndex(list.hovered) : null
                width: list.width
                height: 54
                y: under ? under.y : y
                radius: 12
                color: Theme.alpha(Theme.fg, 0.06)
                opacity: under ? 1 : 0
                Behavior on y {
                    enabled: menuGlide.opacity > 0.01 && !Theme.reducedMotion
                    NumberAnimation { duration: 320; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion }
                }
                Behavior on opacity { NumberAnimation { duration: Theme.reducedMotion ? 0 : 200 } }
            }
            Keys.onReturnPressed: if (currentItem) menu.pick(menu.items[currentIndex].value)
            Keys.onEnterPressed: if (currentItem) menu.pick(menu.items[currentIndex].value)
            Keys.onSpacePressed: if (currentItem) menu.pick(menu.items[currentIndex].value)
            Keys.onPressed: event => {
                keyed = true
                if (event.key === Qt.Key_Home) {
                    currentIndex = 0
                    event.accepted = true
                } else if (event.key === Qt.Key_End) {
                    currentIndex = count - 1
                    event.accepted = true
                }
            }
            delegate: AbstractButton {
                id: option
                required property var modelData
                required property int index
                width: ListView.view.width
                height: 54
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                onHoveredChanged: {
                    if (hovered)
                        list.hovered = index
                    else if (list.hovered === index)
                        list.hovered = -1
                }
                onClicked: menu.pick(modelData.value)
                // :focus-visible: an inset 2 px ring.
                background: Rectangle {
                    radius: 12
                    color: "transparent"
                    border.width: list.keyed && list.activeFocus && list.currentIndex === option.index ? 2 : 0
                    border.color: Theme.alpha(Theme.fg, 0.35)
                }
                contentItem: Item {
                    // .mode-icon: a 30 px tile and its 17 px glyph at .82.
                    Rectangle {
                        x: 8
                        y: 12
                        width: 30
                        height: 30
                        radius: 9
                        color: Theme.alpha(Theme.fg, 0.06)
                        PathIcon {
                            anchors.centerIn: parent
                            width: 17
                            height: 17
                            name: option.modelData.glyph
                            color: Theme.alpha(Theme.fg, 0.82)
                        }
                        PathIcon {
                            anchors.centerIn: parent
                            width: 17
                            height: 17
                            visible: option.modelData.glyph === "ghost"
                            name: "ghost-eyes"
                            color: Theme.appBg
                        }
                    }
                    Column {
                        x: 48
                        width: parent.width - 48 - 10 - 18 - 10
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        CssText {
                            width: parent.width
                            text: option.modelData.title
                            size: 14
                            line: 18
                            cssWeight: 600
                            color: Theme.text
                            elide: Text.ElideRight
                        }
                        CssText {
                            width: parent.width
                            visible: text.length > 0
                            text: option.modelData.hint
                            size: 12.5
                            line: 16
                            color: Theme.secondary
                            elide: Text.ElideRight
                        }
                    }
                    // The check springs in (0.35 s, overshooting).
                    PathIcon {
                        x: parent.width - 10 - 17
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        name: "check-mark"
                        color: Theme.alpha(Theme.fg, 0.9)
                        opacity: option.modelData.checked ? 1 : 0
                        scale: option.modelData.checked ? 1 : 0.6
                    }
                }
            }
        }
    }
}
