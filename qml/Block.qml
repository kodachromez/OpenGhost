import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import OpenGhost.Cpp

// One block of a message, drawn by kind from markdown's allowlist
// (markdown.js's rendering and styles.css). Content is never HTML: text is
// InlineText, code CodeBlock, formulas and diagrams images drawn off the
// GUI thread; quotes and lists hold further levels.
//
// Content exists only near the transcript's viewport (audit P5-02):
// `nearby` is that part of the parent's coordinates (Markdown.qml), unset
// (everything near) outside a lazy transcript row. It is built when the
// block comes within it and released once the block is half that span away
// again, unless it holds an end of its reply's selection (Selection.holds)
// or the focus. A block below
// the window waits its turn with the Pacer, so blocks arriving together
// ahead of the view are built over a few frames, nearest first (audit
// P6-07). Meanwhile the block keeps the height it was last shown at, or an
// estimate, so the view around it does not move.
Item {
    id: block
    required property int index
    required property string kind
    required property var text
    required property var citeRole
    required property string source
    required property string lang
    required property int level
    required property bool flag
    required property bool open
    required property color tone
    required property int section
    required property var tokens
    required property var cells
    required property int columns
    required property var align
    required property var tones
    required property var nested
    required property string path
    required property int gap
    required property int omitted // List items or table rows past markdown's bounds.
    // A fence cut off by the transcript's display bound: its source is a prefix.
    required property bool clipped
    required property var media // Media: its pictures and videos (MediaBlock).
    required property double born // When the row was made (ms): entry motion.
    property var host: null
    property real wideWidth: width
    property bool outer: false
    // Its texts' paths in the reply's selection (RichDocument::units()).
    property string prefix: "r"
    readonly property string unitPath: prefix + path
    readonly property bool inReply: !!host && host.key !== undefined
    // The box Range.getClientRects() gives for the block (the veil's
    // cut-out when the selection takes it whole): a heading's chip, a quote's
    // frame, else the content.
    function enrollBox() {
        const item = content.item
        if (!item || !inReply)
            return
        Selection.enrollBox(kind === "heading" ? item.chip : kind === "quote" ? item.frame : item,
                            host.key, unitPath)
    }
    property var owner: null // The BlockModel holding this block, for estimates.
    readonly property bool wide: outer && flag && (kind === "table" || kind === "diagram")
    // The text style of the level holding the block (Markdown.qml).
    readonly property color textColor: column ? column.textColor : Theme.text
    readonly property int textWeight: column ? column.textWeight : Font.DemiBold
    // CSS max-content in a pull quote: a paragraph's text unwrapped; any
    // other block takes the whole width.
    readonly property real naturalWidth: kind !== "paragraph" ? Infinity
        : content.item ? content.item.format.naturalWidth : 0
    readonly property rect endRect: {
        const item = content.item
        if (!item || item.endRect === undefined || block.omitted > 0)
            return Qt.rect(0, height, 0, 0)
        const end = item.endRect
        return Qt.rect(content.x + end.x, content.y + end.y, end.width, end.height)
    }

    property point nearby: Qt.point(-Infinity, Infinity)
    // Set by check() and admit() rather than bound: building the content
    // changes the height that nearness depends on. The Loader is a focus
    // scope, so its activeFocus says whether a selection is being made
    // inside. Checked once the column has placed the block (Markdown.qml).
    property bool shown: false
    property bool waiting: false // Asked the Pacer to build it.
    property Item column: null
    property bool placed: false
    // In a transcript row, whether the list has positioned it (ChatEntry).
    readonly property bool listed: !host || host.listed !== false
    onListedChanged: check()
    function place() {
        if (placed)
            return
        placed = true
        if (column)
            column.unplaced--
        check()
    }
    function check() {
        if (!placed || !listed)
            return
        const top = nearby.x, bottom = nearby.y, span = bottom - top
        const near = top === -Infinity || y + height >= top && y <= bottom
        const far = top !== -Infinity && (y + height < top - span / 2 || y > bottom + span / 2)
        if (near || (shown || waiting) && (!far || content.activeFocus || holding)) {
            if (!shown) {
                waiting = true
                Pacer.request(block, host && host.eager ? 0 : Theme.ahead(block))
            }
        } else {
            if (waiting)
                Pacer.withdraw(block)
            waiting = false
            shown = false
        }
    }
    // Built now: nested levels are laid out at once too, so the block has
    // its full height before the frame, as a settled row does (ChatEntry).
    function admit() {
        waiting = false
        shown = true
        Theme.settle(content.item)
    }
    onYChanged: check()
    onHeightChanged: check()
    onNearbyChanged: check()
    // Holds an end of its reply's selection: kept while far (check(), which
    // asks only of a built block). Not bound to `shown`: check() sets that
    // when this changes, which would re-evaluate it inside its own change
    // (a binding loop).
    readonly property bool holding: {
        void Selection.revision
        return inReply && Selection.holds(host.key, unitPath)
    }
    onHoldingChanged: if (!holding) check()
    Component.onCompleted: if (column) column.unplaced++
    Component.onDestruction: if (!placed && column) column.unplaced--
    // The content's height when last built: kept while it is not.
    property real measured: -1
    readonly property real built: content.item ? content.item.implicitHeight : -1
    onBuiltChanged: if (built >= 0) measured = built
    onWidthChanged: if (!content.item) measured = -1
    // The window in the content's coordinates, for nested levels and table rows.
    readonly property point inner: Qt.point(nearby.x - y - content.y, nearby.y - y - content.y)

    implicitHeight: gap + (content.item ? content.item.implicitHeight
                           : measured >= 0 ? measured
                           : owner ? owner.estimate(index, content.width) : 0)
                    + (bounded.item ? bounded.item.implicitHeight + 6 : 0)
    height: implicitHeight

    // Entry motion (styles.css), for content built fresh (ChatEntry.fresh()):
    // a heading, code card, quote or formula rises (md-rise: 6 px up from
    // .985, 0.5 s for a heading and 0.45 s otherwise; md-math-in: 6 px up
    // from .98 out of a 5 px blur in 0.7 s); a quote's bar grows down
    // (md-bar, 0.6 s) or a pull quote's mark comes up 3 px (md-quote-mark,
    // 0.5 s ease), and its attribution rises (0.5 s). List markers, table
    // rows and flow chips play their own as they arrive.
    readonly property bool rises: kind === "heading" || kind === "code" || kind === "quote"
                                  || kind === "math"
    function fresh(born) {
        return !!host && !!host.fresh && host.fresh(born)
    }
    Entrance {
        id: rise
        duration: block.kind === "heading" ? 500 : block.kind === "math" ? 700 : 450
    }
    Entrance {
        id: accent
        duration: block.flag ? 500 : 600
        curve: block.flag ? Theme.ease : Theme.motion
    }
    Entrance {
        id: citeRise
        duration: 500
    }
    readonly property real riseValue: rises ? rise.value : 1

    Loader {
        id: content
        objectName: "blockContent"
        active: block.shown
        onActiveFocusChanged: block.check()
        onLoaded: {
            block.enrollBox()
            if (!block.fresh(block.born))
                return
            if (block.rises)
                rise.play()
            if (block.kind === "quote") {
                accent.play()
                citeRise.play()
            }
        }
        y: block.gap
        x: block.wide ? (block.width - block.wideWidth) / 2 : 0
        width: block.wide ? block.wideWidth : block.width
        opacity: block.riseValue
        transform: [
            Scale {
                readonly property real at: (block.kind === "math" ? 0.98 : 0.985)
                                           + (block.kind === "math" ? 0.02 : 0.015) * block.riseValue
                origin.x: block.kind === "heading" ? 0 : content.width / 2
                origin.y: content.height / 2
                xScale: at
                yScale: at
            },
            Translate { y: 6 * (1 - block.riseValue) }
        ]
        layer.enabled: block.kind === "math" && block.riseValue < 1
        layer.effect: MultiEffect {
            blurEnabled: true
            blurMax: 32
            blur: 5 * (1 - block.riseValue) / 8.64 // σ ≈ .27 · blurMax · blur
        }
        sourceComponent: ({
            paragraph: paragraph,
            heading: heading,
            code: code,
            diagram: diagram,
            math: math,
            table: table,
            rule: rule,
            flow: flow,
            list: list,
            quote: quote,
            media: mediaBlock
        })[block.kind] ?? null
    }

    // Past a bound, the rest of a list or table is left out of the view.
    Loader {
        id: bounded
        active: block.omitted > 0
        y: block.height - (item ? item.implicitHeight : 0)
        width: block.width
        sourceComponent: Label {
            objectName: "boundNote"
            text: block.omitted + (block.kind === "table" ? " more rows" : " more items")
                  + " are not shown; Copy the message to get all of them."
            wrapMode: Text.Wrap
            color: Theme.secondary
            font.pixelSize: 13
        }
    }

    Component {
        id: mediaBlock
        MediaBlock {
            width: content.width
            items: block.media
            live: block.open
            host: block.host
        }
    }

    Component {
        id: paragraph
        InlineText {
            objectName: "paragraph"
            width: content.width
            content: block.text
            host: block.host
            unit: block.unitPath + ":t"
            born: block.born
            color: block.textColor
            font.weight: Theme.weight(block.textWeight)
            format.natural: block.column ? block.column.natural : false
        }
    }

    Component {
        id: heading
        Item {
            id: head
            readonly property Item chip: headChip
            readonly property real size: [24, 20.8, 18.24, 16.32, 16.32, 16.32][Math.max(0, block.level - 1)]
            // CSS lines of exactly 1.32em, below Noto Sans's own height: the
            // text's fixed lines (InlineFormat), without the last line's
            // natural overhang that the text item reports.
            implicitHeight: headText.lineCount * size * 1.32 + size * 0.18
            // The tone chip behind the heading's text (.md-h::before). While
            // the reply reveals, it glides as the heading grows (--md-w,
            // 0.24 s).
            Rectangle {
                id: headChip
                objectName: "headingChip"
                width: Math.min(head.width, headText.contentWidth + head.size * 0.84)
                height: head.height
                radius: head.size * 0.34
                color: Theme.alpha(block.tone, 0.15)
                Behavior on width {
                    enabled: !Theme.reducedMotion && !!block.host && !!block.host.rich
                             && block.host.rich.revealing
                    NumberAnimation {
                        duration: 240
                        easing.type: Easing.Bezier
                        easing.bezierCurve: [0.25, 0.8, 0.3, 1, 1, 1]
                    }
                }
            }
            FontMetrics {
                id: headMetrics
                font: headText.font
            }
            InlineText {
                id: headText
                objectName: "heading"
                x: head.size * 0.42
                width: head.width - head.size * 0.84
                // A fixed line puts the baseline 4/5 down; CSS puts it half
                // the leading below the (rounded) ascent: moved by the difference.
                topPadding: head.size * 0.08 - (0.8 * head.size * 1.32
                    - (head.size * 1.32 + Math.round(headMetrics.ascent) - Math.round(headMetrics.descent)) / 2)
                bottomPadding: head.size * 0.1
                pixelSize: head.size
                lineHeight: 1.32
                font.weight: Theme.weight(Font.ExtraBold)
                font.letterSpacing: -0.012 * head.size
                color: block.tone
                content: block.text
                host: block.host
                unit: block.unitPath + ":t"
                born: block.born
            }
        }
    }

    Component {
        id: code
        CodeBlock {
            source: block.source
            lang: block.lang
            tokens: block.tokens
            tone: block.tone
            art: block.flag
            clipped: block.clipped
            host: block.host
            born: block.born
            unitPath: block.unitPath
        }
    }

    Component {
        id: diagram
        DiagramBlock {
            source: block.source
            lang: block.lang
            live: block.open
            clipped: block.clipped
            tones: block.tones
            tone: block.tone
            section: block.section
            column: block.width
            fresh: block.open || block.fresh(block.born)
            path: block.path
            host: block.host
            unit: block.inReply ? block.unitPath + ":g" : ""
        }
    }

    Component {
        id: math
        Item {
            id: formula
            // One U+FFFC of the reply's selection, copied as $$…$$: the
            // drawing (or its source when not drawn) is selected whole.
            readonly property string unit: block.unitPath + ":m"
            readonly property point selected: {
                void Selection.revision
                return block.inReply ? Selection.range(block.host.key, unit) : Qt.point(-1, -1)
            }
            function enroll(item) {
                if (item && block.inReply)
                    Selection.enroll(item, block.host.key, unit)
            }
            Component.onCompleted: enroll(image)
            implicitHeight: image.ok ? image.height + 18 : fallback.active ? fallback.implicitHeight : 44
            Flickable {
                anchors.fill: parent
                contentWidth: Math.max(width, image.width + 8)
                contentHeight: height
                clip: contentWidth > width
                interactive: contentWidth > width
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                RichImage {
                    id: image
                    objectName: "math"
                    kind: "math"
                    source: block.source
                    live: block.open
                    x: Math.max(4, (formula.width - width) / 2)
                    y: 8
                    width: implicitWidth
                    height: implicitHeight
                    visible: ok
                }
                // Chromium tints a selected image over it.
                SelectionWash {
                    x: image.x
                    y: image.y
                    width: image.width
                    height: image.height
                    visible: image.visible
                    target: image
                    range: formula.selected
                    color: Qt.rgba(Theme.selection.r, Theme.selection.g, Theme.selection.b, Selection.wash)
                }
            }
            // Refused or not drawn: the source stays readable.
            Loader {
                id: fallback
                active: !image.ok && !image.pending && image.error.length > 0
                width: parent.width
                onLoaded: formula.enroll(item)
                sourceComponent: Label {
                    objectName: "mathSource"
                    width: formula.width
                    text: block.source
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    font.family: Theme.mono
                    font.pointSize: Theme.points(13.5)
                    color: Theme.text
                }
            }
        }
    }

    Component {
        id: table
        TableBlock {
            cells: block.cells
            columns: block.columns
            align: block.align
            tone: block.tone
            host: block.host
            nearby: block.inner
            born: block.born
            unitPath: block.unitPath
        }
    }

    Component {
        id: rule
        Item { implicitHeight: 0 }
    }

    Component {
        id: flow
        Flow {
            spacing: 8
            Repeater {
                model: block.cells
                delegate: Row {
                    id: step
                    required property int index
                    required property var modelData
                    readonly property color hue: block.tones[index % block.tones.length]
                    spacing: 8
                    // md-chip (0.5 s, k × 110 ms late: from .8 out of a 4 px
                    // blur) and md-arrow (0.45 s, 50 ms before its chip: 6 px
                    // from the left).
                    Entrance { id: chipIn; duration: 500; delay: step.index * 110 }
                    Entrance { id: arrowIn; duration: 450; delay: step.index * 110 - 50 }
                    Component.onCompleted: {
                        if (block.fresh(block.born)) {
                            chipIn.play()
                            arrowIn.play()
                        }
                    }
                    Label {
                        visible: step.index > 0
                        height: chip.height
                        verticalAlignment: Text.AlignVCenter
                        text: "⟶"
                        color: Theme.alpha(Theme.strong, 0.35)
                        font.pixelSize: 14
                        opacity: arrowIn.value
                        transform: Translate { x: -6 * (1 - arrowIn.value) }
                    }
                    Rectangle {
                        id: chip
                        objectName: "flowChip"
                        width: chipText.contentWidth + 24
                        height: 30
                        radius: height / 2
                        color: Theme.alpha(step.hue, 0.15)
                        opacity: chipIn.value
                        scale: 0.8 + 0.2 * chipIn.value
                        layer.enabled: chipIn.value < 1
                        layer.effect: MultiEffect {
                            blurEnabled: true
                            blurMax: 32
                            blur: 4 * (1 - chipIn.value) / 8.64
                        }
                        InlineText {
                            id: chipText
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.min(implicitWidth, block.width - 24)
                            wrapMode: TextEdit.NoWrap
                            pixelSize: 14.5
                            lineHeight: 0
                            font.weight: Theme.weight(Font.Black)
                            color: step.hue
                            content: step.modelData
                            host: block.host
                            unit: block.unitPath + ":f" + step.index
                            waves: false // .md-flow is data-nowave: its chips pop instead.
                        }
                    }
                }
            }
        }
    }

    Component {
        id: list
        Column {
            objectName: block.flag ? "orderedList" : "list"
            Repeater {
                model: block.nested
                delegate: Item {
                    id: entry
                    required property int number
                    required property int task
                    required property int depth
                    required property int gap
                    required property var nested
                    required property double born
                    required property string path
                    // Its marker pops in as the item arrives (md-pop, 0.4 s from .4).
                    Entrance { id: pop; duration: 400 }
                    Component.onCompleted: if (block.fresh(entry.born)) pop.play()
                    readonly property bool ordered: block.flag
                    // ul 22 px, ol 32 px, a task list 26 px (styles.css).
                    readonly property real indent: ordered ? 32 : entry.task >= 0 ? 26 : 22
                    readonly property real line: 16 * 1.65
                    width: parent ? parent.width : 0
                    height: gap + body.height
                    Rectangle { // A bullet: solid, hollow when nested.
                        objectName: "bullet"
                        visible: !entry.ordered && entry.task < 0
                        opacity: pop.value
                        scale: 0.4 + 0.6 * pop.value
                        x: entry.indent - 15
                        y: entry.gap + entry.line / 2 - 3
                        width: 6
                        height: 6
                        radius: 3
                        color: entry.depth > 0 ? "transparent" : block.tone
                        border.width: entry.depth > 0 ? 1.5 : 0
                        border.color: Theme.alpha(block.tone, 0.85)
                    }
                    Rectangle { // An ordered item's number badge.
                        visible: entry.ordered
                        opacity: pop.value
                        scale: 0.4 + 0.6 * pop.value
                        y: entry.gap + entry.line / 2 - 10
                        width: Math.max(21, number.implicitWidth + 10)
                        height: 20
                        radius: 7
                        color: Theme.alpha(block.tone, 0.15)
                        Label {
                            id: number
                            anchors.centerIn: parent
                            text: entry.number
                            color: block.tone
                            font.pointSize: Theme.points(11.5)
                            font.weight: Theme.weight(Font.Black)
                        }
                    }
                    Rectangle { // A task box, filled and ticked when done.
                        visible: entry.task >= 0
                        opacity: pop.value
                        scale: 0.4 + 0.6 * pop.value
                        y: entry.gap + entry.line / 2 - 8
                        width: 16
                        height: 16
                        radius: 5
                        color: entry.task > 0 ? block.tone : "transparent"
                        border.width: entry.task > 0 ? 0 : 1.5
                        border.color: Theme.alpha(block.tone, 0.75)
                        PathIcon {
                            anchors.fill: parent
                            visible: entry.task > 0
                            name: "tick"
                            color: Theme.chatBg
                        }
                    }
                    Loader {
                        id: body
                        x: entry.indent
                        y: entry.gap
                        width: entry.width - entry.indent
                        // The window from the start, so no block is built before it applies.
                        Component.onCompleted: {
                            setSource("Markdown.qml", {
                                blocks: entry.nested, host: block.host, wideWidth: width,
                                nearby: entry.inner, textColor: block.textColor,
                                textWeight: block.textWeight, prefix: block.prefix
                            })
                            // The li's box (from the marker's indent).
                            if (block.inReply)
                                Selection.enrollBox(body, block.host.key, block.prefix + entry.path)
                        }
                    }
                    readonly property point inner: Qt.point(block.inner.x - entry.y - body.y,
                                                            block.inner.y - entry.y - body.y)
                    Binding {
                        target: body.item
                        property: "nearby"
                        value: entry.inner
                    }
                }
            }
        }
    }

    Component {
        id: quote
        Item {
            id: box
            objectName: callout ? "callout" : pull ? "quote" : "innerQuote"
            readonly property bool callout: block.lang.length > 0
            readonly property bool pull: block.flag
            readonly property Item frame: quoteFrame
            readonly property real padTop: pull ? 11 : 10
            readonly property real padLeft: pull ? 40 : 22
            readonly property real padRight: pull ? 18 : 16
            readonly property real padBottom: pull ? 12 : 10
            readonly property real contentBottom: Math.max(body.y + body.height,
                closeMark.visible ? closeMark.y + closeMark.height : 0)
            implicitHeight: contentBottom + (cite.visible ? 6 + cite.height : 0) + padBottom
            // A pull quote is as wide as its text (fit-content), up to the column.
            readonly property real natural: Math.max(body.item ? body.item.naturalWidth : 0,
                cite.item ? cite.item.implicitWidth : 0,
                closeMark.visible ? closeMark.x + closeMark.width - padLeft : 0)
            readonly property rect endRect: cite.visible || !closeMark.visible
                ? Qt.rect(0, height, 0, 0)
                : Qt.rect(closeMark.x, closeMark.y, closeMark.width, closeMark.height)
            Rectangle {
                id: quoteFrame
                objectName: "quoteBox"
                width: box.pull ? Math.min(box.width, Math.round(box.natural + box.padLeft + box.padRight))
                                : box.width
                height: box.height
                radius: 14
                color: box.pull ? Theme.alpha(Theme.strong, 0.035) : Theme.alpha(block.tone, 0.07)
                border.width: box.pull ? 1 : 0
                // The inset contour (.06) lies over the fill (.035), not beside it.
                border.color: Theme.alpha(Theme.strong, 0.035 + 0.06 * (1 - 0.035))
            }
            Rectangle { // The bar of a quote or callout.
                objectName: "quoteBar"
                visible: !box.pull
                x: 10
                y: 11
                width: 3
                height: box.height - 22
                radius: 3
                color: block.tone
                transform: Scale { yScale: accent.value }
            }
            Label { // A pull quote's mark.
                visible: box.pull
                x: 14
                y: 13
                text: "“"
                color: Theme.alpha(block.tone, 0.85)
                font.family: "serif"
                font.pixelSize: 32
                lineHeight: 0.8
                opacity: accent.value
                transform: Translate { y: 3 * (1 - accent.value) }
            }
            Label {
                id: closeMark
                objectName: "quoteClose"
                visible: !box.callout
                // Follow the last laid-out line, not the frame's corner or
                // the first paragraph. With no room, flow onto a new line;
                // contentBottom reserves its full height before the cite.
                readonly property rect end: body.item ? body.item.endRect : Qt.rect(0, 0, 0, 0)
                readonly property bool nextLine: end.height <= 0 || end.x + end.width + 2 + width > body.width
                x: body.x + (nextLine ? 0 : end.x + end.width + 2)
                y: body.y + (nextLine ? Math.max(body.height, end.y + end.height) : end.y)
                text: "”"
                color: Theme.alpha(block.tone, 0.85)
                font.family: "serif"
                font.pixelSize: 32
                lineHeight: 0.8
                opacity: accent.value
                transform: Translate { y: 3 * (1 - accent.value) }
            }
            Row {
                id: title
                visible: box.callout
                x: box.padLeft
                y: box.padTop
                height: visible ? 20 : 0
                spacing: 7
                PathIcon {
                    width: 15
                    height: 15
                    anchors.verticalCenter: parent.verticalCenter
                    name: block.lang === "tip" ? "tip"
                        : block.lang === "warning" || block.lang === "caution" ? "warning" : "note"
                    color: block.tone
                }
                SelectableLabel {
                    objectName: "calloutTitle"
                    anchors.verticalCenter: parent.verticalCenter
                    text: ({ note: "NOTE", tip: "TIP", important: "IMPORTANT", warning: "WARNING",
                             caution: "CAUTION" })[block.lang] ?? ""
                    host: block.host
                    unit: block.unitPath + ":h"
                    color: block.tone
                    font.pointSize: Theme.points(12.5)
                    font.weight: Theme.weight(Font.Black)
                    font.letterSpacing: 0.5
                }
            }
            Loader {
                id: body
                x: box.padLeft
                y: box.padTop + title.height + (title.visible ? 8 : 0)
                width: box.width - box.padLeft - box.padRight
                // .md-quote: .86 at 500; a quote within: .78; a callout: primary.
                Component.onCompleted: setSource("Markdown.qml", {
                    blocks: block.nested, host: block.host, wideWidth: width,
                    nearby: box.inner, natural: box.pull,
                    textColor: box.pull ? Theme.alpha(Theme.strong, 0.86)
                             : box.callout ? Theme.text : Theme.alpha(Theme.strong, 0.78),
                    textWeight: box.pull ? Font.Medium : block.textWeight,
                    prefix: block.prefix
                })
            }
            readonly property point inner: Qt.point(block.inner.x - body.y, block.inner.y - body.y)
            Binding {
                target: body.item
                property: "nearby"
                value: box.inner
            }
            Loader { // A pull quote's attribution.
                id: cite
                active: box.pull && block.text && block.text.plain.length > 0
                visible: active
                x: box.padLeft
                y: box.contentBottom + 6
                opacity: citeRise.value
                transform: [
                    Scale {
                        origin.x: cite.width / 2
                        origin.y: cite.height / 2
                        xScale: 0.985 + 0.015 * citeRise.value
                        yScale: xScale
                    },
                    Translate { y: 6 * (1 - citeRise.value) }
                ]
                sourceComponent: Row {
                    spacing: 8
                    Rectangle {
                        width: 12
                        height: 1
                        anchors.verticalCenter: parent.verticalCenter
                        color: Theme.tertiary
                    }
                    InlineText {
                        width: Math.min(implicitWidth, box.width - box.padLeft - box.padRight - 20)
                        wrapMode: TextEdit.NoWrap
                        pixelSize: 13
                        lineHeight: 1.38
                        color: Theme.alpha(Theme.strong, 0.72)
                        content: block.text
                        host: block.host
                        unit: block.unitPath + ":n"
                        born: block.born
                    }
                    Label {
                        visible: block.citeRole && block.citeRole.plain.length > 0
                        text: "·"
                        color: Theme.tertiary
                        font.pixelSize: 13
                    }
                    InlineText {
                        visible: block.citeRole && block.citeRole.plain.length > 0
                        width: Math.min(implicitWidth, box.width - box.padLeft - box.padRight - 80)
                        wrapMode: TextEdit.NoWrap
                        pixelSize: 13
                        lineHeight: 1.38
                        font.weight: Theme.weight(Font.Medium)
                        color: Theme.secondary
                        content: block.citeRole
                        host: block.host
                        unit: block.unitPath + ":r"
                        born: block.born
                    }
                }
            }
        }
    }
}
