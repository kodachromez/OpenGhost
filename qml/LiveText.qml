import QtQuick
import QtQuick.Controls
import OpenGhost.Native

// Read-only plain text that follows a streamed string by editing its document:
// an append inserts only the new suffix and a bounded front trim removes only
// the dropped prefix, so selection and layout survive streaming. Any other
// change (retry, settlement, a final result) replaces the whole text.
TextArea {
    property string content
    // UTF-16 units the producer has dropped from content's front so far.
    property double trimmed: 0
    property string shown
    property double shownTrimmed: 0
    // Emitted after the front trim removed `height` px of text.
    signal dropped(real height)
    // The window controller; a finished selection is copied exactly to the
    // clipboard (not the primary selection), as OpenGhost's auto-copy does.
    property var frontend: null
    // A right click asks for the selection menu.
    signal menuRequested()
    property bool dragging: false
    property bool syncing: false
    // New text waves in (TextWave) when one is given.
    property var wave: null
    // CSS line-height in pixels; 0 keeps the font's own. Each line is this
    // tall (Theme.lines), its extra room above the text; CSS splits the room
    // evenly, so the text moves up half of it.
    property real lineHeight: 0
    readonly property real halfLeading: lineHeight > 0 ? Theme.halfLeading(font, lineHeight) : 0
    onLineHeightChanged: Theme.lines(textDocument, 0, lineHeight)

    background: null
    // Fusion's TextArea adds 4 px on the left; the text starts at its CSS box.
    leftPadding: padding
    selectionColor: Theme.selection
    selectedTextColor: Theme.strong
    topPadding: -halfLeading
    bottomPadding: halfLeading
    textFormat: TextEdit.PlainText
    readOnly: true
    selectByMouse: true
    Component.onCompleted: sync()
    // Deferred so that content and trimmed, updated one role at a time, are
    // read together.
    onContentChanged: Qt.callLater(sync)
    onTrimmedChanged: Qt.callLater(sync)
    onPressed: dragging = true
    onReleased: {
        dragging = false
        copySelection()
    }
    // Keyboard selection; streaming's own selection repair is not a new selection.
    onSelectedTextChanged: if (!dragging && !syncing) Qt.callLater(copySelection)
    Keys.onPressed: function(event) {
        if (event.matches(StandardKey.Copy)) {
            event.accepted = true
            copySelection()
        }
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: parent.menuRequested()
    }

    function copySelection() {
        if (frontend && selectionStart !== selectionEnd)
            frontend.copySelection(textDocument, selectionStart, selectionEnd)
    }

    function sync() {
        syncing = true
        const next = content
        const cut = trimmed - shownTrimmed
        const kept = shown.length - cut
        // "\r\n" is one document position; keep string and document offsets equal.
        if (next !== shown && cut >= 0 && kept >= 0 && next.length >= kept
                && next.startsWith(cut ? shown.slice(cut) : shown)
                && !(cut && shown.slice(0, cut).includes("\r"))
                && !(shown.endsWith("\r") && next.charAt(kept) === "\n")) {
            const height = cut ? positionToRectangle(cut).y - positionToRectangle(0).y : 0
            if (cut)
                remove(0, cut)
            if (next.length > kept) {
                const at = length, from = selectionStart, to = selectionEnd
                const cursor = cursorPosition
                insert(at, next.slice(kept))
                if (lineHeight > 0)
                    Theme.lines(textDocument, at, lineHeight)
                if (wave)
                    wave.grew(at)
                // Text inserted at the cursor or a selection edge would extend it.
                if (to === at) {
                    if (from === to)
                        cursorPosition = cursor
                    else
                        select(cursor === from ? to : from, cursor)
                }
            }
            if (height > 0)
                dropped(height)
        } else if (next !== shown) {
            let same = 0
            while (same < shown.length && same < next.length && shown[same] === next[same])
                same++
            text = next
            if (lineHeight > 0)
                Theme.lines(textDocument, 0, lineHeight)
            if (wave)
                wave.grew(same)
        }
        shown = next
        shownTrimmed = trimmed
        syncing = false
    }
}
