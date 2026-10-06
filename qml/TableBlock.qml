import QtQuick
import QtQuick.Window
import OpenGhost.Cpp

// A Markdown table (.md-table): header cells, then rows, each cell styled
// inline text. Columns take their natural width, share any room left, and
// shrink (wrapping) down to a floor; past that the table scrolls sideways.
// As Block.qml does for blocks, a row's cells exist only near the
// transcript's viewport; a row not built keeps its last or estimated height.
Item {
    id: table
    objectName: "table"
    required property var cells
    required property int columns
    required property var align
    required property color tone
    property var host: null
    // Its block's path in the reply's selection: a cell is ":c<row>.<column>"
    // and a row's box ":c<row>" (RichDocument::units()).
    property string unitPath: ""
    readonly property bool inReply: !!host && host.key !== undefined && unitPath.length > 0
    // The part near the viewport, in this table's coordinates (Block.qml).
    property point nearby: Qt.point(-Infinity, Infinity)
    readonly property int rows: columns > 0 ? Math.floor(cells.length / columns) - 1 : 0
    // When the block's row was made, and each table row since (ms): a row
    // built soon after it arrived plays its entry motion, the header md-rise
    // (6 px up from .985) and a body row md-row (5 px up), 0.45 s each.
    property double born: 0
    property var rowBorn: []
    onRowsChanged: {
        if (!complete)
            return
        const next = rowBorn.slice(0, rows + 1)
        while (next.length <= rows)
            next.push(Date.now())
        rowBorn = next
    }

    implicitHeight: view.height

    TextMetrics {
        id: measure
        objectName: "tableMeasure"
        font.pointSize: Theme.points(14.5)
        font.weight: Theme.weight(Font.Black)
        onFontChanged: table.remeasure()
    }
    // Each cell's unwrapped line advances and each column's natural width,
    // padding included. They depend on the cells' text, the font and the
    // device pixel ratio, not on the table's width, so they are kept while
    // this table exists and measured again only when one of those changes.
    property var lines: []
    property var natural: []
    property bool stale: true
    // Cells measured so far (instrumentation for tests).
    property int measurements: 0
    // Column widths share this table's width among the natural ones; each
    // row's height estimate wraps its cells' lines at those widths.
    property var widths: []
    property var estimates: []
    // Nothing is laid out until the table is complete, so however many of
    // its inputs are set while it is made, it measures and lays out once.
    property bool complete: false
    readonly property real ratio: Screen.devicePixelRatio
    onCellsChanged: remeasure()
    onRatioChanged: remeasure()
    onWidthChanged: layout()
    Component.onCompleted: {
        rowBorn = new Array(rows + 1).fill(born)
        complete = true
        layout()
    }
    function remeasure() {
        stale = true
        layout()
    }
    function layout() {
        if (!complete)
            return
        if (stale)
            measureCells()
        const next = columnWidths()
        const heights = []
        for (let r = 0; r <= rows; ++r) {
            let most = 1
            for (let c = 0; c < columns; ++c) {
                const room = Math.max(1, (next[c] ?? 0) - 32)
                const wrapped = (lines[r * columns + c] ?? []).reduce(
                    (n, w) => n + (r === 0 ? 1 : Math.max(1, Math.ceil(w / room))), 0)
                most = Math.max(most, wrapped)
            }
            // Padding, lines and the cells' 1 px bottom border (transparent in the last row).
            heights.push(r === 0 ? 13 + most * 12.5 * 1.5 : 25 + most * 14.5 * 1.5)
        }
        // Unchanged values keep the cells' and rows' bindings still.
        if (!same(next, widths))
            widths = next
        if (!same(heights, estimates))
            estimates = heights
    }
    function measureCells() {
        lines = cells.map(cell => {
            const out = []
            for (const line of (cell ? cell.plain : "").split("\n")) {
                measure.text = line
                out.push(measure.advanceWidth)
            }
            return out
        })
        measurements += cells.length
        const widest = []
        for (let c = 0; c < columns; ++c) {
            let most = 0
            for (let r = 0; r <= rows; ++r)
                most = Math.max(most, ...(lines[r * columns + c] ?? [0]))
            widest.push(Math.min(most, 600) + 32)
        }
        natural = widest
        stale = false
    }
    function same(a, b) {
        return a.length === b.length && a.every((v, i) => v === b[i])
    }
    function columnWidths() {
        const total = natural.reduce((a, b) => a + b, 0)
        const room = table.width
        if (total <= room)
            return natural.map(w => w + (room - total) * w / total)
        const floor = natural.map(w => Math.min(w, 140))
        const floors = floor.reduce((a, b) => a + b, 0)
        if (floors >= room)
            return floor
        const shrink = (room - floors) / (total - floors)
        return natural.map((w, c) => floor[c] + (w - floor[c]) * shrink)
    }

    Flickable {
        id: view
        width: table.width
        height: grid.height
        contentWidth: grid.width
        contentHeight: height
        clip: contentWidth > width
        interactive: contentWidth > width
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        Column {
            id: grid
            width: table.widths.reduce((a, b) => a + b, 0)
            // Rows compare their place with the window once laid out
            // (Markdown.qml), and again once a changed height is laid out.
            // Not while a row height binding is being written: from the y
            // the column has yet to update, the check could build the row,
            // and its cells set that height again (a binding loop).
            property int unplaced: 0
            property var resized: []
            onPositioningComplete: {
                const rows = resized
                if (unplaced <= 0 && rows.length === 0)
                    return
                resized = []
                if (unplaced > 0) {
                    for (const child of children) {
                        if (child.place)
                            child.place()
                    }
                }
                for (const row of rows) {
                    if (row.check) // Not a row since destroyed.
                        row.check()
                }
                forceLayout()
            }
            Repeater {
                model: table.rows + 1
                delegate: Item {
                    id: row
                    required property int index
                    readonly property bool header: index === 0
                    width: grid.width
                    height: cellsRow.item ? cellsRow.item.height
                          : measured >= 0 ? measured : table.estimates[index] ?? 0
                    // Built near the viewport, in its turn, and released far
                    // from it (Block.qml).
                    property bool shown: false
                    property bool waiting: false
                    property bool placed: false
                    readonly property bool listed: !table.host || table.host.listed !== false
                    onListedChanged: check()
                    function place() {
                        if (placed)
                            return
                        placed = true
                        grid.unplaced--
                        check()
                    }
                    function check() {
                        if (!placed || !listed)
                            return
                        const top = table.nearby.x, bottom = table.nearby.y, span = bottom - top
                        const near = top === -Infinity || y + height >= top && y <= bottom
                        const far = top !== -Infinity
                                    && (y + height < top - span / 2 || y > bottom + span / 2)
                        if (near || (shown || waiting) && (!far || cellsRow.activeFocus || holding)) {
                            if (!shown) {
                                waiting = true
                                Pacer.request(row, table.host && table.host.eager ? 0
                                                                                   : Theme.ahead(row))
                            }
                        } else {
                            if (waiting)
                                Pacer.withdraw(row)
                            waiting = false
                            shown = false
                        }
                    }
                    function admit() {
                        waiting = false
                        shown = true
                        Theme.settle(cellsRow.item)
                    }
                    readonly property point nearby: table.nearby
                    onYChanged: check()
                    // The tr's box, for the veil's cut-out.
                    Component.onCompleted: {
                        grid.unplaced++
                        if (table.inReply)
                            Selection.enrollBox(row, table.host.key, table.unitPath + ":c" + index)
                    }
                    onHeightChanged: if (placed) grid.resized.push(row)
                    // Holds an end of the reply's selection: kept while far
                    // (not bound to `shown`, which check() sets: Block.qml).
                    readonly property bool holding: {
                        void Selection.revision
                        return table.inReply
                               && Selection.holds(table.host.key, table.unitPath + ":c" + index)
                    }
                    onHoldingChanged: if (!holding) check()
                    onNearbyChanged: check()
                    Component.onDestruction: if (!placed && grid) grid.unplaced--
                    Entrance { id: rowIn; duration: 450 }
                    readonly property double born: table.rowBorn[index] ?? Date.now()
                    property real measured: -1
                    readonly property real built: cellsRow.item ? cellsRow.item.height : -1
                    onBuiltChanged: if (built >= 0) measured = built
                    onWidthChanged: if (!cellsRow.item) measured = -1
                    Loader {
                        id: cellsRow
                        active: row.shown
                        onActiveFocusChanged: row.check()
                        onLoaded: {
                            if (table.host && table.host.fresh && table.host.fresh(row.born))
                                rowIn.play()
                        }
                        opacity: row.header ? 1 : rowIn.value
                        transform: Translate { y: row.header ? 0 : 5 * (1 - rowIn.value) }
                        sourceComponent: Row {
                            Repeater {
                                model: table.columns
                                delegate: Item {
                                    id: cell
                                    required property int index
                                    readonly property int alignment: table.align[index] ?? 0
                                    width: table.widths[index] ?? 0
                                    height: body.height + (row.header ? 12 : 24) + 1
                                    // A header cell rises on its own (.md-table th).
                                    opacity: row.header ? rowIn.value : 1
                                    transform: [
                                        Scale {
                                            readonly property real at: row.header
                                                ? 0.985 + 0.015 * rowIn.value : 1
                                            origin.x: cell.width / 2
                                            origin.y: cell.height / 2
                                            xScale: at
                                            yScale: at
                                        },
                                        Translate { y: row.header ? 6 * (1 - rowIn.value) : 0 }
                                    ]
                                    InlineText {
                                        id: body
                                        x: 16
                                        y: row.header ? 2 : 12
                                        width: cell.width - 32
                                        wrapMode: row.header ? TextEdit.NoWrap : TextEdit.Wrap
                                        horizontalAlignment: cell.alignment === 2 ? TextEdit.AlignRight
                                                           : cell.alignment === 1 ? TextEdit.AlignHCenter
                                                                                  : TextEdit.AlignLeft
                                        pixelSize: row.header ? 12.5 : 14.5
                                        lineHeight: 1.5
                                        font.weight: Theme.weight(row.header ? Font.Bold
                                                   : cell.index === 0 ? Font.Black : Font.DemiBold)
                                        color: row.header ? Theme.secondary
                                             : cell.index === 0 ? Theme.strong : Theme.text
                                        content: table.cells[row.index * table.columns + cell.index]
                                        host: table.host
                                        unit: table.unitPath.length > 0
                                              ? table.unitPath + ":c" + row.index + "." + cell.index : ""
                                        born: row.born
                                    }
                                }
                            }
                        }
                    }
                    Rectangle { // Row rules; the last row has none.
                        visible: row.index < table.rows
                        y: row.height - 1
                        width: row.width
                        height: 1
                        color: Theme.alpha(Theme.strong, row.header ? 0.12 : 0.055)
                    }
                }
            }
        }
    }
}
