import QtQuick
import QtQuick.Effects
import OpenGhost.Native

// Pictures and videos shown in a reply (media-embed.js and styles.css
// .md-media): the pictures as a stack to leaf through, with the caption and
// the source of the one on top, and a card with a preview for every video.
// Bytes come only through MediaLoader, under its rules: a picture from a
// trusted preview place loads by itself, any other is a plate that loads it
// on a click. What did not load stays a link, so nothing the reply showed is
// lost. While the reply is still writing its pictures they wait as one
// quiet plate. Links open through the desktop (host.frontend.openLink).
Item {
    id: media
    objectName: "mediaBlock"
    required property var items // BlockModel's media role.
    property bool live: false
    property var host: null

    readonly property var pictures: items.filter(item => !item.video && /^https?:\/\//i.test(item.src))
    readonly property var videos: {
        const kept = new Map();
        for (const item of items)
            if (item.video && item.id && item.id.length > 0)
                kept.set(item.src + "\n" + item.caption, item);
        return Array.from(kept.values());
    }
    // Keyed reconciliation, as mount()/arrange(): appending surrounding text
    // or another video must not destroy an existing card and its load state.
    ListModel {
        id: videoItems
        dynamicRoles: true
    }
    function syncVideos() {
        for (let n = videoItems.count - 1; n >= 0; --n)
            if (!videos.some(v => v.src + "\n" + v.caption === videoItems.get(n).key))
                videoItems.remove(n);
        for (let n = 0; n < videos.length; ++n) {
            const key = videos[n].src + "\n" + videos[n].caption;
            let at = n;
            while (at < videoItems.count && videoItems.get(at).key !== key)
                ++at;
            if (at === videoItems.count)
                videoItems.insert(n, {
                    key: key,
                    value: videos[n]
                });
            else if (at !== n)
                videoItems.move(at, n, 1);
        }
    }
    onVideosChanged: Qt.callLater(syncVideos)
    Component.onCompleted: syncVideos()
    implicitHeight: parts.implicitHeight

    function open(url) {
        if (url && host && host.frontend)
            host.frontend.openLink(url);
    }

    readonly property color mediaBg: Theme.light ? Qt.rgba(238 / 255, 238 / 255, 238 / 255, 1) : Qt.rgba(20 / 255, 20 / 255, 20 / 255, 1)

    Column {
        id: parts
        width: media.width
        spacing: 18

        Loader {
            id: gallery
            objectName: "mediaGalleryLoader"
            width: parts.width
            active: media.pictures.length > 0
            visible: active
            sourceComponent: media.live ? waiting : stackOf
        }

        Item {
            id: videoRow
            objectName: "mediaVideos"
            visible: media.videos.length > 0
            width: parts.width
            readonly property bool many: media.videos.length > 1
            // .md-videos.is-many: repeat(auto-fill, minmax(max(170px, min(210px, 46%)), 1fr)), gap 20px 14px.
            readonly property real least: Math.max(170, Math.min(210, width * 0.46))
            readonly property int columns: many ? Math.max(1, Math.floor((width + 14) / (least + 14))) : 1
            readonly property real cell: many ? (width - 14 * (columns - 1)) / columns : Math.min(380, width)
            implicitHeight: cards.implicitHeight
            height: implicitHeight
            Grid {
                id: cards
                columns: parent.columns
                columnSpacing: 14
                rowSpacing: 20
                Repeater {
                    model: videoItems
                    delegate: VideoCard {
                        required property var value
                        item: value
                        width: videoRow.cell
                    }
                }
            }
        }
    }

    // .md-gallery-wait: a quiet plate keeping the pictures' place.
    component Plate: Rectangle {
        objectName: "mediaWait"
        width: Math.min(parent ? parent.width : 380, 380)
        height: width * 3 / 4
        radius: 18
        color: Theme.alpha(Theme.fg, 0.04 + 0.035 * pulse)
        border.width: 1
        border.color: Theme.alpha(Theme.fg, 0.07)
        property real pulse: 0
        SequentialAnimation on pulse {
            running: !Theme.reducedMotion
            loops: Animation.Infinite
            NumberAnimation {
                to: 1
                duration: 900
                easing.type: Easing.InOutQuad
            }
            NumberAnimation {
                to: 0
                duration: 900
                easing.type: Easing.InOutQuad
            }
        }
    }

    Component {
        id: waiting
        Item {
            implicitHeight: plate.height
            Plate {
                id: plate
            }
        }
    }

    // .link-chip: a link shown as its host, with the local globe.
    component Chip: Rectangle {
        id: chip
        readonly property bool selectionControl: true
        property string link
        property string label
        // 0.875em of where it stands: 14 px in a row of its own, 12.25 in a caption.
        property real pixelSize: 14
        property int textWeight: Font.Medium
        width: row.width + 15
        height: pixelSize * 1.65
        radius: 8
        color: Theme.alpha(Theme.fg, hit.containsMouse ? 0.14 : 0.08)
        Behavior on color {
            enabled: !Theme.reducedMotion
            ColorAnimation {
                duration: 200
            }
        }
        Accessible.role: Accessible.Link
        Accessible.name: link
        Row {
            id: row
            x: 6
            height: parent.height
            spacing: 6
            PathIcon {
                width: 14
                height: 14
                anchors.verticalCenter: parent.verticalCenter
                name: "link-globe"
                color: Theme.alpha(Theme.fg, (hit.containsMouse ? 1 : 0.9) * 0.65)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: chip.label
                textFormat: Text.PlainText
                color: Theme.alpha(Theme.fg, hit.containsMouse ? 1 : 0.9)
                font.pointSize: Theme.points(chip.pixelSize)
                font.weight: Theme.weight(chip.textWeight)
            }
        }
        MouseArea {
            id: hit
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: media.open(chip.link)
        }
        ButtonTip {
            text: chip.link
        }
    }

    Component {
        id: stackOf
        Column {
            id: shelf
            objectName: "mediaGallery"
            spacing: 10
            width: gallery.width
            // Pictures the person asked for, by address.
            property var allowed: ({})
            // What the gallery shows, set once every picture it waits for
            // has come or failed (media-embed.js draw()).
            property var loaded: []
            property var lost: []
            property bool drawn: false
            property int front: 0
            readonly property var held: media.pictures.filter(image => !MediaLoader.trusted(image.src) && !shelf.allowed[image.src])
            function draw() {
                const shown = media.pictures.filter(image => MediaLoader.trusted(image.src) || shelf.allowed[image.src]);
                let waiting = false;
                for (const image of shown) {
                    const state = MediaLoader.state(image.src);
                    if (state === "")
                        MediaLoader.load(image.src, !!shelf.allowed[image.src]);
                    if (MediaLoader.state(image.src) === "loading")
                        waiting = true;
                }
                if (waiting)
                    return;
                const ready = shown.filter(image => MediaLoader.state(image.src) === "ready");
                const nextLoaded = ready.map(image => {
                    const size = MediaLoader.size(image.src);
                    return {
                        url: image.src,
                        source: MediaLoader.source(image.src),
                        width: size.width,
                        height: size.height,
                        caption: image.caption,
                        href: image.href,
                        label: image.label
                    };
                });
                const nextLost = shown.filter(image => MediaLoader.state(image.src) !== "ready");
                // What already stands is not touched: the stack keeps its place
                // while other pictures and previews load around it.
                if (JSON.stringify(nextLoaded) !== JSON.stringify(loaded)) {
                    loaded = nextLoaded;
                    front = 0;
                }
                if (JSON.stringify(nextLost) !== JSON.stringify(lost))
                    lost = nextLost;
                drawn = true;
            }
            function allow(src) {
                const next = Object.assign({}, allowed);
                next[src] = true;
                allowed = next;
                draw();
            }
            Component.onCompleted: draw()
            // Later, not inside the change that called for it; gone with the gallery.
            Timer {
                id: redraw
                interval: 0
                onTriggered: shelf.draw()
            }
            Connections {
                target: MediaLoader
                function onChanged() {
                    redraw.restart();
                }
            }
            Connections {
                target: media
                function onPicturesChanged() {
                    redraw.restart();
                }
            }

            // Before the first pictures came, a plate keeps their place.
            Plate {
                visible: !shelf.drawn && shelf.held.length < media.pictures.length
            }
            Item {
                objectName: "mediaStackPlace"
                visible: shelf.loaded.length > 0
                width: shelf.width
                // The pictures behind fan out below the one on top (margin 6 / 26 px).
                height: stackLoader.item ? stackLoader.item.height + (shelf.loaded.length > 1 ? 32 : 0) : 0
                Loader {
                    id: stackLoader
                    active: shelf.loaded.length > 0
                    y: shelf.loaded.length > 1 ? 6 : 0
                    sourceComponent: MediaStack {
                        id: stack
                        images: shelf.loaded
                        available: shelf.width
                        onIndexChanged: shelf.front = index
                        // A click that was not a drag opens the page the picture is from.
                        onOpened: k => media.open(shelf.loaded[k].href || shelf.loaded[k].url)
                        opacity: 0
                        Component.onCompleted: appear.start()
                        ParallelAnimation {
                            id: appear
                            NumberAnimation {
                                target: stack
                                property: "opacity"
                                from: 0
                                to: 1
                                duration: Theme.reducedMotion ? 0 : 320
                                easing.type: Easing.OutQuint
                            }
                            NumberAnimation {
                                target: lift
                                property: "y"
                                from: Theme.reducedMotion ? 0 : 6
                                to: 0
                                duration: Theme.reducedMotion ? 0 : 320
                                easing.type: Easing.OutQuint
                            }
                        }
                        transform: Translate {
                            id: lift
                        }
                    }
                }
            }
            // .md-gallery-caption: the caption and source of the picture on top.
            Item {
                id: captionLine
                objectName: "mediaCaption"
                readonly property var image: shelf.loaded[shelf.front] || shelf.loaded[0] || null
                visible: !!image && (image.caption.length > 0 || image.href.length > 0)
                width: shelf.width
                readonly property bool wrapped: captionText.visible && sourceChip.visible && captionText.width + 10 + sourceChip.width > width
                height: wrapped ? captionText.height + 4 + sourceChip.height : Math.max(captionText.visible ? captionText.height : 0, sourceChip.visible ? sourceChip.height : 0)
                Text {
                    id: captionText
                    objectName: "mediaCaptionText"
                    visible: text.length > 0
                    width: Math.min(implicitWidth, shelf.width)
                    text: parent.image ? parent.image.caption : ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.secondary
                    font.pointSize: Theme.points(14)
                    font.weight: Theme.weight(Font.Medium)
                    lineHeight: 21
                    lineHeightMode: Text.FixedHeight
                    height: lineCount * lineHeight
                    topPadding: Theme.halfLeading(font, lineHeight)
                }
                Item {
                    id: sourceChip
                    objectName: "mediaSource"
                    property alias link: sourceLink.link
                    visible: !!parent.image && parent.image.href.length > 0
                    width: sourceLink.width + 2
                    height: sourceLink.height + 2
                    x: captionLine.wrapped || !captionText.visible ? 0 : captionText.width + 10
                    y: captionLine.wrapped ? captionText.height + 4 : 0
                    Chip {
                        id: sourceLink
                        x: 1
                        y: 2 // Inline link baseline, within the caption's line box.
                        pixelSize: 12.25
                        link: captionLine.image ? captionLine.image.href : ""
                        label: captionLine.image ? captionLine.image.label : ""
                    }
                }
            }
            // What waits to be asked for, and what did not load, stay links.
            Flow {
                objectName: "mediaHeld"
                visible: shelf.held.length > 0 || shelf.lost.length > 0
                width: shelf.width
                spacing: 8
                Repeater {
                    model: shelf.held
                    delegate: Rectangle {
                        id: ask
                        objectName: "mediaAsk"
                        readonly property bool selectionControl: true
                        required property var modelData
                        width: Math.min(shelf.width, askRow.width + 20)
                        height: 30
                        radius: 10
                        color: Theme.alpha(Theme.fg, askHit.containsMouse ? 0.12 : 0.07)
                        Behavior on color {
                            enabled: !Theme.reducedMotion
                            ColorAnimation {
                                duration: 200
                            }
                        }
                        Accessible.role: Accessible.Button
                        Accessible.name: "Show the picture from " + modelData.host
                        Row {
                            id: askRow
                            x: 9
                            height: parent.height
                            spacing: 8
                            PathIcon {
                                width: 15
                                height: 15
                                anchors.verticalCenter: parent.verticalCenter
                                name: "media-picture"
                                color: Theme.alpha(Theme.fg, 0.9 * 0.75)
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.min(implicitWidth, Math.max(40, shelf.width - 60 - hostText.implicitWidth))
                                text: ask.modelData.caption || "Picture"
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                color: Theme.alpha(Theme.fg, 0.9)
                                font.pointSize: Theme.points(14)
                                font.weight: Theme.weight(Font.Medium)
                            }
                            Text {
                                id: hostText
                                anchors.verticalCenter: parent.verticalCenter
                                text: ask.modelData.host
                                color: Theme.secondary
                                font.pointSize: Theme.points(14)
                                font.weight: Theme.weight(Font.Medium)
                            }
                        }
                        MouseArea {
                            id: askHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: shelf.allow(ask.modelData.src)
                        }
                        ButtonTip {
                            text: "Show the picture from " + ask.modelData.host
                        }
                    }
                }
                Repeater {
                    model: shelf.lost
                    delegate: Item {
                        objectName: "mediaLost"
                        required property var modelData
                        width: lostLink.width + 2
                        height: 26.4 // The inherited 16 px / 1.65 line box.
                        Chip {
                            id: lostLink
                            x: 1
                            y: 2
                            textWeight: Font.DemiBold // .markdown a.link-chip inherits 600.
                            link: parent.modelData.link
                            label: parent.modelData.label
                        }
                    }
                }
            }
        }
    }

    // .md-video: its preview with a play mark on it, its name under it, and
    // who made it. The widest preview first, the narrow one for a video too
    // old or small for it; one that has neither keeps its link and loses the
    // plate.
    component VideoCard: Item {
        id: card
        objectName: "mediaVideo"
        readonly property bool selectionControl: true
        property var item
        implicitHeight: body.implicitHeight
        height: implicitHeight
        readonly property var sources: MediaLoader.thumbnails(item.id)
        property var metadata: ({
                title: "",
                by: ""
            })
        readonly property string displayTitle: item.title || metadata.title || ""
        readonly property string displayBy: item.by || metadata.by || "YouTube"
        function lookup() {
            VideoTitles.request(item.id);
            metadata = VideoTitles.info(item.id);
        }
        Connections {
            target: VideoTitles
            function onChanged() {
                card.metadata = VideoTitles.info(card.item.id);
            }
        }
        onItemChanged: {
            tried = 0;
            lookup();
        }
        property int tried: 0
        readonly property string current: tried < sources.length ? sources[tried] : ""
        // Where its current preview's load stands, and whether it is a
        // preview at all (YouTube's stand-in for none is NoThumb wide).
        property string phase: ""
        property bool loaded: false
        readonly property bool missing: tried >= sources.length
        function next() {
            phase = current.length ? MediaLoader.state(current) : "";
            loaded = phase === "ready" && MediaLoader.size(current).width > 120;
            if (!current.length)
                return;
            if (phase === "")
                MediaLoader.load(current, false);
            else if (phase === "failed" || (phase === "ready" && !loaded))
                tried++;
        }
        // Later, not inside the change that called for it; gone with the card.
        Timer {
            id: later
            interval: 0
            onTriggered: card.next()
        }
        Connections {
            target: MediaLoader
            function onChanged() {
                later.restart();
            }
        }
        onCurrentChanged: later.restart()
        Component.onCompleted: {
            later.restart();
            lookup();
        }

        activeFocusOnTab: true
        Keys.onReturnPressed: media.open(item.src)
        Keys.onEnterPressed: media.open(item.src)
        Accessible.role: Accessible.Link
        Accessible.name: (displayTitle || "YouTube video") + ", " + item.src
        Accessible.onPressAction: media.open(item.src)

        Column {
            id: body
            width: card.width
            Item {
                id: thumb
                objectName: "mediaVideoThumb"
                visible: !card.missing
                width: card.width
                height: card.width * 9 / 16
                readonly property bool lifted: hit.containsMouse && !Theme.reducedMotion
                transform: Translate {
                    y: thumb.lifted ? -2 : 0
                    Behavior on y {
                        NumberAnimation {
                            duration: 350
                            easing.type: Easing.OutQuint
                        }
                    }
                }
                BoxShadow { // 0 8px 22px (.26), hovered 0 14px 30px (.34) × --shadow
                    anchors.fill: parent
                    radius: 14
                    blur: thumb.lifted ? 30 : 22
                    offsetY: thumb.lifted ? 14 : 8
                    color: Qt.rgba(0, 0, 0, (thumb.lifted ? 0.34 : 0.26) * Theme.shadow)
                }
                Item {
                    id: thumbnailPlane
                    anchors.fill: parent
                    Item {
                        anchors.fill: parent
                        layer.enabled: true
                        layer.effect: MultiEffect {
                            maskEnabled: true
                            maskThresholdMin: 0.5
                            maskSpreadAtMin: 0.5
                            maskSource: thumbMask
                        }
                        Rectangle {
                            anchors.fill: parent
                            color: media.mediaBg
                        }
                        Image {
                            objectName: "mediaVideoImage"
                            anchors.fill: parent
                            source: card.loaded ? MediaLoader.source(card.current) : ""
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: false
                            cache: false
                            smooth: true
                            mipmap: true
                            opacity: source.toString().length ? 1 : 0
                            scale: thumb.lifted ? 1.03 : 1
                            Behavior on opacity {
                                enabled: !Theme.reducedMotion
                                NumberAnimation {
                                    duration: 400
                                }
                            }
                            Behavior on scale {
                                enabled: !Theme.reducedMotion
                                NumberAnimation {
                                    duration: 600
                                    easing.type: Easing.OutQuint
                                }
                            }
                        }
                    }
                } // masked thumbnail; sample its non-layer parent, not a shared layer texture.
                Rectangle {
                    id: thumbMask
                    anchors.fill: parent
                    radius: 14
                    visible: false
                    layer.enabled: true
                }
                Rectangle { // .md-video-thumb::after
                    anchors.fill: parent
                    radius: 14
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.alpha(Theme.fg, 0.07)
                }
                MediaGlass { // .md-video-play
                    objectName: "mediaPlayGlass"
                    backdrop: thumbnailPlane
                    anchors.centerIn: parent
                    width: 46
                    height: 46
                    radius: 23
                    color: Qt.rgba(0, 0, 0, hit.containsMouse ? 0.5 : 0.38)
                    border.width: 0.5
                    border.color: Qt.rgba(1, 1, 1, 0.28)
                    scale: Theme.reducedMotion ? 1 : hit.pressed ? 0.94 : hit.containsMouse ? 1.08 : 1
                    Behavior on scale {
                        enabled: !Theme.reducedMotion
                        NumberAnimation {
                            duration: 300
                            easing.type: Easing.OutQuint
                        }
                    }
                    PathIcon {
                        anchors.centerIn: parent
                        width: 22
                        height: 22
                        name: "media-play"
                        color: "white"
                    }
                }
                MediaGlass { // .md-video-time
                    objectName: "mediaTimeGlass"
                    backdrop: thumbnailPlane
                    visible: card.item.time.length > 0
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 8
                    height: 18
                    radius: 9
                    width: time.implicitWidth + 14
                    color: Qt.rgba(0, 0, 0, 0.42)
                    Text {
                        id: time
                        anchors.centerIn: parent
                        text: card.item.time
                        color: Qt.rgba(1, 1, 1, 0.94)
                        font.pointSize: Theme.points(11.5)
                        font.weight: Theme.weight(Font.DemiBold)
                        font.features: {
                            "tnum": 1
                        }
                    }
                }
            }
            Item { // .md-video-title margin-top
                width: 1
                height: card.missing ? 0 : 10
                visible: title.visible
            }
            Text {
                id: title
                objectName: "mediaVideoTitle"
                visible: text.length > 0
                width: card.width
                text: card.displayTitle
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                color: Theme.alpha(Theme.fg, 0.92)
                font.pointSize: Theme.points(14.4)
                font.hintingPreference: Font.PreferNoHinting
                font.weight: Theme.weight(Font.DemiBold)
                lineHeight: 14.4 * 1.35 // CSS line-height: 1.35
                lineHeightMode: Text.FixedHeight
                topPadding: Theme.halfLeading(font, lineHeight)
            }
            Item {
                width: 1
                height: 5
            }
            Row { // .md-video-meta
                width: card.width
                spacing: 6
                PathIcon {
                    width: 14
                    height: 14
                    anchors.verticalCenter: parent.verticalCenter
                    name: "link-globe"
                    color: Theme.alpha(Theme.fg, 0.65 * 0.6)
                }
                Text {
                    objectName: "mediaVideoBy"
                    width: Math.min(implicitWidth, card.width - 20)
                    text: card.displayBy
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.secondary
                    font.pointSize: Theme.points(12.8)
                    font.weight: Theme.weight(Font.Medium)
                    lineHeight: 12.8 * 1.4
                    lineHeightMode: Text.FixedHeight
                    height: lineHeight
                    topPadding: Theme.halfLeading(font, lineHeight)
                }
            }
        }
        MouseArea {
            id: hit
            objectName: "mediaVideoHit"
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: media.open(card.item.src)
        }
        ButtonTip {
            text: card.item.src
        }
    }
}
