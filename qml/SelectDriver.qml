import QtQuick
import OpenGhost.Cpp

// Drags the selection across the transcript (selection.h): while a range is
// dragged, the window's pointer moves (Selection.moved, which outlive the row
// the press began in, released once far from the view) carry its focus to
// the text under the pointer, in any row. Held past the transcript's top, or
// past its bottom less scroll-padding-bottom (OpenGhost's composer space +
// 16 px), the transcript scrolls each frame by the pointer's distance past
// that edge, as Chromium autoscrolls a selection, through every row to the
// conversation's ends, and the focus follows; a scrolling box the press
// was in scrolls the same way past its own edges. Let go, the range settles (a
// reply's menu, or none: Selection.release()) and is copied, as OpenGhost's
// port copies a finished selection.
Item {
    id: driver
    required property Flickable view
    property real scrollPadding: 0
    property var frontend: null
    // The pointer in the transcript's coordinates, which stay put while it scrolls.
    property point pointer: Qt.point(0, 0)
    // The scrolling box the press was in, if any: held
    // past its top or bottom, it scrolls the same way, and the focus follows
    // through its text.
    property Flickable inner: null
    readonly property real innerPast: {
        if (!inner || !inner.visible || inner.contentHeight <= inner.height + 0.5)
            return 0
        void inner.contentY
        const at = inner.mapFromItem(view, pointer.x, pointer.y).y
        return at < 0 ? at : at > inner.height ? at - inner.height : 0
    }

    function extend() {
        const at = Selection.hitView(view, pointer.x, pointer.y)
        if (at.row !== undefined)
            Selection.drag(at.row, at.unit, at.offset, at.outside === true)
    }
    Connections {
        target: Selection
        function onPressed(x, y) {
            driver.pointer = driver.view.mapFromItem(null, x, y)
            driver.inner = Selection.scrollerAt(driver.view, driver.pointer.x, driver.pointer.y)
        }
        function onMoved(x, y) {
            driver.pointer = driver.view.mapFromItem(null, x, y)
            driver.extend()
        }
        function onReleased(x, y) {
            driver.pointer = driver.view.mapFromItem(null, x, y)
            Selection.release()
            if (Selection.active && driver.frontend)
                driver.frontend.copy(Selection.text)
        }
    }
    FrameAnimation {
        running: Selection.dragging
                 && (driver.pointer.y < 0 || driver.pointer.y > driver.view.height - driver.scrollPadding
                     || driver.innerPast !== 0)
        onTriggered: driver.scrollStep()
    }
    function scrollStep() {
        if (inner && innerPast !== 0) {
            const most = Math.max(0, inner.contentHeight - inner.height)
            const next = Math.max(0, Math.min(most, inner.contentY + innerPast))
            if (Math.abs(next - inner.contentY) >= 0.01) {
                inner.contentY = next
                extend()
            }
        }
        const v = view, y = pointer.y, bottom = v.height - scrollPadding
        const distance = y > bottom ? y - bottom : y < 0 ? y : 0
        const most = v.originY + v.contentHeight - v.height
        const next = Math.max(v.originY, Math.min(most, v.contentY + distance))
        if (Math.abs(next - v.contentY) < 0.01)
            return
        v.contentY = next
        // A scroll the reader made: the follow latch and the kept place follow it.
        if (typeof v.atEnd === "function") {
            v.follow = v.atEnd()
            v.mark()
        }
        extend()
    }
}
