import QtQuick
import OpenGhost.Cpp

// The pointer over a transcript row's texts (a reply, user message or note),
// as Chromium's over OpenGhost's
// .message: a press starts the conversation's one selection (selection.h)
// at the nearest text, a drag carries it across blocks and on across rows
// (SelectDriver, which the window's pointer moves reach), two and three
// clicks take a word or a block, and Shift extends. Presses on controls
// under it (copy buttons, a diagram's tools or a code well's scroll bar)
// pass through, and the wheel
// reaches what scrolls under it. A right press asks for the selection menu.
//
// A press on a link is the link's, as Chromium's is: it selects nothing.
// Let go within Selection.dragDistance (4 px) on both axes, however long
// held and with any modifier, it is a click: the selection goes and the
// link opens (each click of a double or triple click opens it, and selects
// no word). Moved that far on either axis, the link is dragged (its URL; the
// range's text when the link lies inside the range) and nothing is selected
// or opened. A middle click on a link opens it too.
//
// A single press inside the range (no Shift) holds it, as Chromium's does:
// moved Selection.dragDistance on either axis, the range's text is dragged
// and the range stays; let go before that, the range goes as a click there
// leaves it.
//
// Keys, once it has the focus: Ctrl+C copies the selection; Escape lets a
// shown selection go (else stops a running reply, as Escape anywhere in
// OpenGhost does); Shift+arrows extend it (Ctrl: by words), across rows;
// Ctrl+A takes the row's text.
MouseArea {
    id: area
    objectName: "selectArea"
    required property var host     // The row: key, kind, frontend, menu().
    // A reply's RichDocument; user messages and notes get their plain text
    // from the transcript.
    property var document: host && host.kind === "assistant" ? host.rich : null
    property Item content: null    // What lies under it.
    property Flickable view: null  // The transcript, when there is one.
    acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
    preventStealing: true
    activeFocusOnTab: false

    readonly property bool inRow: !!host && host.key !== undefined
    function register() {
        if (inRow)
            Selection.surface(area, host.key, document)
    }
    Component.onCompleted: register()
    onDocumentChanged: register()

    // Two and three presses close together in time and place: a word, a block.
    property int clicks: 0
    property double lastPress: 0
    property point lastAt: Qt.point(-1e9, -1e9)
    // A press on a link, until it is released or dragged: its href, where
    // it was pressed and whether it lies inside the range.
    property string link: ""
    property point linkFrom: Qt.point(0, 0)
    property bool linkSelected: false
    // A press inside the range, until it is released or dragged: where it
    // was pressed, and the point of the text under it.
    property var held: null
    property point heldFrom: Qt.point(0, 0)

    function copy() {
        if (Selection.active && host.frontend)
            host.frontend.copy(Selection.text)
    }

    onPressed: mouse => {
        if (!inRow || (content && Selection.control(content, mouse.x, mouse.y))) {
            mouse.accepted = false
            return
        }
        if (mouse.button === Qt.RightButton) {
            if (host.menu)
                host.menu(null)
            return
        }
        const href = Selection.linkAt(host.key, area, mouse.x, mouse.y)
        if (mouse.button === Qt.MiddleButton || href.length > 0) {
            if (href.length === 0) {
                mouse.accepted = false
                return
            }
            link = href
            linkFrom = Qt.point(mouse.x, mouse.y)
            const on = Selection.hit(host.key, area, mouse.x, mouse.y)
            linkSelected = on.unit !== undefined && Selection.covers(host.key, on.unit, on.offset)
            forceActiveFocus()
            return
        }
        const now = Date.now()
        const near = Math.abs(mouse.x - lastAt.x) <= Qt.styleHints.startDragDistance
                     && Math.abs(mouse.y - lastAt.y) <= Qt.styleHints.startDragDistance
        clicks = near && now - lastPress < Qt.styleHints.mouseDoubleClickInterval
                 ? clicks % 3 + 1 : 1
        lastPress = now
        lastAt = Qt.point(mouse.x, mouse.y)
        const at = Selection.hit(host.key, area, mouse.x, mouse.y)
        if (at.unit === undefined) {
            mouse.accepted = false
            return
        }
        forceActiveFocus()
        const shift = (mouse.modifiers & Qt.ShiftModifier) !== 0
        if (mouse.button === Qt.LeftButton && clicks === 1 && !shift && at.outside !== true
                && Selection.covers(host.key, at.unit, at.offset)) {
            held = at
            heldFrom = Qt.point(mouse.x, mouse.y)
            return
        }
        Selection.press(host.key, document, at.unit, at.offset, clicks, shift, at.outside === true)
    }
    onPositionChanged: mouse => {
        if (held && (mouse.buttons & Qt.LeftButton)
                && (Math.abs(mouse.x - heldFrom.x) >= Selection.dragDistance
                    || Math.abs(mouse.y - heldFrom.y) >= Selection.dragDistance)) {
            held = null
            Selection.dragLink(area, "", true) // The range's text.
            if (Selection.active)
                Selection.release()
            return
        }
        if (link.length === 0 || !(mouse.buttons & Qt.LeftButton))
            return
        if (Math.abs(mouse.x - linkFrom.x) < Selection.dragDistance
                && Math.abs(mouse.y - linkFrom.y) < Selection.dragDistance)
            return
        const href = link
        link = ""
        Selection.dragLink(area, href, linkSelected)
        // Its drag over, a range still there shows its menu again
        // (selection-menu.js on pointerup).
        if (Selection.active)
            Selection.release()
    }
    onReleased: mouse => {
        if (held) {
            // A click in the range: it collapses there, as any click does.
            const at = held
            held = null
            Selection.press(host.key, document, at.unit, at.offset, 1, false, false)
            Selection.release()
            return
        }
        if (link.length === 0)
            return // A selection's release is SelectDriver's.
        const href = link
        link = ""
        if (mouse.button === Qt.LeftButton)
            Selection.clear()
        if (host.frontend)
            host.frontend.openLink(href)
    }
    onCanceled: {
        link = ""
        held = null
    }

    Keys.onPressed: event => {
        if (event.matches(StandardKey.Copy)) {
            event.accepted = true
            copy()
        } else if (event.key === Qt.Key_Escape) {
            if (Selection.shown) {
                event.accepted = true
                Selection.clear()
            } else if (host.frontend && host.frontend.canCancel) {
                event.accepted = true
                host.frontend.cancel()
            }
        } else if (event.matches(StandardKey.SelectAll)) {
            event.accepted = true
            Selection.selectAll(host.key, document)
            copy()
        } else if ((event.modifiers & Qt.ShiftModifier) && Selection.active) {
            const words = (event.modifiers & Qt.ControlModifier) !== 0
            if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                event.accepted = true
                Selection.step(event.key === Qt.Key_Right ? 1 : -1, words)
                copy()
            } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                event.accepted = true
                // The nearest line above or below the focus's, in any text of
                // any row, at the focus's x.
                const space = view ? view : area
                const caret = Selection.caretIn(space)
                const down = event.key === Qt.Key_Down
                for (let step = 1; caret.height > 0 && step < 800; step += 2) {
                    const y = down ? caret.y + caret.height + step : caret.y - step
                    const at = view ? Selection.hitView(view, caret.x, y, true)
                                    : Selection.hitInside(host.key, area, caret.x, y)
                    if (at.unit !== undefined
                            && (down ? at.line > caret.y + 0.5 : at.line < caret.y - 0.5)) {
                        Selection.extendTo(at.row !== undefined ? at.row : host.key, at.unit, at.offset)
                        copy()
                        break
                    }
                }
            }
        }
    }
}
