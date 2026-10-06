import QtQuick
import OpenGhost.Cpp

// One level of a message's blocks: the message itself, or a quote's or list
// item's contents. Nested levels load this file again through a Loader;
// markdown bounds how deep they go.
Column {
    id: view
    required property var blocks // A BlockModel.
    property var host: null      // The row: key, frontend, selection menu.
    // Its texts' paths in the reply's selection start "r" (blocks) or "p"
    // (plain rows): RichDocument::units().
    property string prefix: "r"
    property real wideWidth: width // Room for wide tables and diagrams.
    property bool outer: false
    // The wide-diagram rule also outranks :last-child's zero bottom margin.
    // The message collapses it with the following toolbar's margin.
    readonly property real trailingMargin: {
        const last = rows.count ? rows.itemAt(rows.count - 1) : null
        return last && last.wide && last.kind === "diagram" ? 22 : 0
    }
    // The text's colour and weight here: a quote's own (.md-quote,
    // blockquote, .md-callout), else the message's.
    property color textColor: Theme.text
    property int textWeight: Font.DemiBold
    // Set in a pull quote, which shrinks to its text: naturalWidth is this
    // level's CSS max-content, Infinity where a block takes the whole width.
    property bool natural: false
    readonly property real naturalWidth: {
        let widest = 0
        for (const child of children) {
            if (child.naturalWidth !== undefined)
                widest = Math.max(widest, child.naturalWidth)
        }
        return widest
    }
    // The part of this level near the transcript's viewport, from nearby.x
    // to nearby.y in its own coordinates (one value, so both ends change
    // together): only blocks there are built (Block.qml). Unset, all are.
    property point nearby: Qt.point(-Infinity, Infinity)
    // Blocks created but not yet positioned: their y is not theirs until the
    // column lays them out, so they compare it with the window only then.
    // Those built on being placed are laid out at once, not at the next polish.
    property int unplaced: 0
    onPositioningComplete: {
        if (unplaced > 0) {
            for (const child of children) {
                if (child.place)
                    child.place()
            }
            forceLayout()
        }
    }

    // The last block owns the endpoint, including paragraph breaks. A
    // non-text block ends on a new line rather than over its painted content.
    readonly property rect endRect: {
        const last = rows.count > 0 ? rows.itemAt(rows.count - 1) : null
        if (!last)
            return Qt.rect(0, height, 0, 0)
        const end = last.endRect
        return Qt.rect(last.x + end.x, last.y + end.y, end.width, end.height)
    }
    Repeater {
        id: rows
        model: view.blocks
        delegate: Block {
            host: view.host
            width: view.width
            wideWidth: view.wideWidth
            outer: view.outer
            owner: view.blocks
            column: view
            nearby: view.nearby
            prefix: view.prefix
        }
    }
}
