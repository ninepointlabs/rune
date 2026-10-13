import QtQuick
import Rune

// The document as pages: sheets the size of the page, with the text laid out
// on them by the document itself (it is paginated at the page size, its
// margins the page margins), each page's header and footer, and their
// in-place editors. In a view narrower than a page, everything scales down
// to fit. Which part is being edited is the window's state (editingPart,
// editingPage); see Main.qml's editPart() / finishPartEditing().
Flickable {
    id: flick

    required property var appRoot   // the Main.qml Window instance
    required property DocumentController controller
    readonly property var theme: appRoot.theme
    // The body's TextEdit, and the header's and footer's.
    readonly property alias editor: editor
    readonly property alias headerEditor: headerEditor
    readonly property alias footerEditor: footerEditor

    // The text of the body, header or footer changed.
    signal edited()

    contentWidth: width
    contentHeight: pageColumn.height * zoom + 2 * gutter
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    readonly property real gutter: 24
    readonly property real zoom: Math.min(1, (width - 2 * gutter) / pageColumn.width)

    // Keep the cursor in view while typing (`r` in the editor's coordinates).
    function ensureVisible(r) {
        const rect = editor.mapToItem(flick.contentItem, r.x, r.y, r.width, r.height)
        if (contentY >= rect.y)
            contentY = rect.y
        else if (contentY + height <= rect.y + rect.height)
            contentY = rect.y + rect.height - height
    }

    // While a header or footer is edited, a click beside the pages also
    // goes back to the body (on the pages: partEditingShield).
    MouseArea {
        width: flick.width
        height: flick.contentHeight
        visible: flick.appRoot.partEditing
        onPressed: flick.appRoot.finishPartEditing()
    }

    Item {
        id: pageColumn
        objectName: "pageColumn"
        readonly property size page: flick.controller.pageSizePx
        // Between sheets, in the margins on either side of a page break.
        readonly property real gap: 32
        x: Math.max(flick.gutter, (flick.width - width * flick.zoom) / 2)
        y: flick.gutter
        width: page.width
        height: Math.max(flick.controller.pageCount * page.height, editor.implicitHeight)
        scale: flick.zoom
        transformOrigin: Item.TopLeft

        Repeater {
            model: flick.controller.pageCount
            Rectangle {
                required property int index
                objectName: "pageSheet"
                y: index * pageColumn.page.height + (index > 0 ? pageColumn.gap / 2 : 0)
                width: pageColumn.width
                height: pageColumn.page.height - (index > 0 ? pageColumn.gap / 2 : 0)
                        - (index < flick.controller.pageCount - 1 ? pageColumn.gap / 2 : 0)
                color: flick.appRoot.pageColor
                border.color: Qt.alpha(flick.theme.foreground, 0.2)
                border.width: 1

                // A shadow under the sheet's lower edge, so each page
                // visibly ends.
                Rectangle {
                    z: -1
                    x: 3
                    y: 4
                    width: parent.width
                    height: parent.height
                    color: Qt.alpha("black", 0.35)
                }
            }
        }

        // Each page's header and footer, as that page shows them, and
        // where the pointer is over an empty one, how to add it.
        Repeater {
            model: flick.controller.pageCount
            Item {
                id: pageParts
                required property int index
                readonly property var layout: flick.controller.pageLayout
                // The margin under the pointer, if any (see partHotspots).
                readonly property string hovered: partHotspots.count > index ? partHotspots.itemAt(index).hovered : ""
                y: index * pageColumn.page.height
                width: pageColumn.width
                height: pageColumn.page.height

                PagePartView {
                    objectName: "headerView"
                    documentController: flick.controller
                    part: "header"
                    page: pageParts.index + 1
                    x: flick.appRoot.mmToPx(pageParts.layout.left)
                    y: flick.controller.headerYPx
                    width: pageColumn.width - x - flick.appRoot.mmToPx(pageParts.layout.right)
                    height: flick.controller.headerHeightPx
                    color: Qt.alpha(flick.theme.foreground, 0.75)
                    visible: flick.controller.hasHeader && !(flick.appRoot.editingPart === "header" && flick.appRoot.editingPage === pageParts.index + 1)
                }
                PagePartView {
                    objectName: "footerView"
                    documentController: flick.controller
                    part: "footer"
                    page: pageParts.index + 1
                    x: flick.appRoot.mmToPx(pageParts.layout.left)
                    y: flick.controller.footerYPx
                    width: pageColumn.width - x - flick.appRoot.mmToPx(pageParts.layout.right)
                    height: flick.controller.footerHeightPx
                    color: Qt.alpha(flick.theme.foreground, 0.75)
                    visible: flick.controller.hasFooter && !(flick.appRoot.editingPart === "footer" && flick.appRoot.editingPage === pageParts.index + 1)
                }
                Text {
                    readonly property bool header: pageParts.hovered === "header"
                    visible: !flick.appRoot.partEditing && pageParts.hovered !== ""
                             && (header ? !flick.controller.hasHeader : !flick.controller.hasFooter)
                    x: flick.appRoot.mmToPx(pageParts.layout.left)
                    // Centred in the margin, as the header or footer would be.
                    y: header ? (flick.appRoot.mmToPx(pageParts.layout.top) - height) / 2
                              : pageParts.height - (flick.appRoot.mmToPx(pageParts.layout.bottom) + height) / 2
                    text: header ? "Double-click to add a header" : "Double-click to add a footer"
                    color: Qt.alpha(flick.theme.foreground, 0.4)
                    font.pixelSize: 13
                }
            }
        }

        // The rest of the last page, below the text: a click there
        // puts the cursor at the end, as in the text's own margin.
        MouseArea {
            objectName: "belowText"
            y: editor.height
            width: parent.width
            height: Math.max(0, parent.height - editor.height)
            cursorShape: Qt.IBeamCursor
            onClicked: {
                editor.forceActiveFocus()
                editor.cursorPosition = editor.length
            }
        }

        TextEdit {
            id: editor
            objectName: "editor"
            width: pageColumn.width
            // Just the text: tying it to the pages would loop (the page
            // count comes from this layout). Clicks below it are the
            // MouseArea's above.
            height: implicitHeight
            padding: 0 // the margins are the document's (see DocumentController::pageLayout)
            focus: true
            // A .docx open replaces the document when it finishes; edits
            // made meanwhile would be lost (see DocumentController::openFile).
            // ... and while the AI writes into it (one undo step, see
            // DocumentController::beginStreamedEdit()).
            readOnly: flick.controller.loading || flick.controller.streamingEdit
            opacity: flick.appRoot.partEditing ? 0.45 : 1
            selectByMouse: true
            persistentSelection: true
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            color: flick.theme.foreground
            selectionColor: flick.theme.accent
            selectedTextColor: flick.theme.background

            onTextChanged: flick.edited()
            onCursorPositionChanged: flick.appRoot.syncSelection()
            onSelectionStartChanged: flick.appRoot.syncSelection()
            onSelectionEndChanged: flick.appRoot.syncSelection()
            onCursorRectangleChanged: flick.ensureVisible(cursorRectangle)

            // Undo/redo go through the controller rather than the TextEdit's
            // built-in handling, which knows nothing of pending formats or
            // the toolbar. Printable text is inserted by the controller too,
            // which groups typing into word-sized undo steps; see
            // DocumentController::typeText(). Return, Backspace, Delete and
            // input-method text stay with the TextEdit.
            Keys.onPressed: event => {
                if (event.matches(StandardKey.Undo))
                    flick.controller.undo()
                else if (event.matches(StandardKey.Redo))
                    flick.controller.redo()
                else if (!(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                         && event.text.length > 0 && event.text >= " " && event.text !== "\x7f"
                         && flick.controller.typeText(event.text))
                    ; // inserted
                else
                    return
                event.accepted = true
            }

            // Tab / Shift+Tab move between table cells, or else nest list
            // items; elsewhere Tab is left to the TextEdit, which inserts a
            // tab character.
            Keys.onTabPressed: event => {
                if (flick.controller.isInTable())
                    flick.controller.nextTableCell()
                else if (flick.controller.isInBulletList() || flick.controller.isInNumberedList())
                    flick.controller.demoteListItem()
                else
                    event.accepted = false
            }
            Keys.onBacktabPressed: event => {
                if (flick.controller.isInTable())
                    flick.controller.previousTableCell()
                else if (flick.controller.isInBulletList() || flick.controller.isInNumberedList())
                    flick.controller.promoteListItem()
                else
                    event.accepted = false
            }
        }

        // Over each page's top and bottom margins: a double-click there
        // edits that page's header or footer; a click or drag places the
        // body's cursor, as one in the margin would anyway.
        Repeater {
            id: partHotspots
            model: flick.controller.pageCount
            Item {
                id: hotspot
                required property int index
                // "header" or "footer" under the pointer, else "".
                readonly property string hovered: headerArea.containsMouse ? "header"
                                                : footerArea.containsMouse ? "footer" : ""
                y: index * pageColumn.page.height
                width: pageColumn.width
                height: pageColumn.page.height
                visible: !flick.appRoot.partEditing

                // A press or drag in the margin works on the body as one
                // there would (the editor's own clicks don't reach it).
                function pressAt(area, mouse) {
                    const p = area.mapToItem(editor, mouse.x, mouse.y)
                    const position = editor.positionAt(p.x, p.y)
                    editor.forceActiveFocus()
                    if (mouse.modifiers & Qt.ShiftModifier)
                        editor.moveCursorSelection(position)
                    else
                        editor.cursorPosition = position
                }
                function dragAt(area, mouse) {
                    const p = area.mapToItem(editor, mouse.x, mouse.y)
                    editor.moveCursorSelection(editor.positionAt(p.x, p.y))
                }

                MouseArea {
                    id: headerArea
                    objectName: "headerArea"
                    width: parent.width
                    height: flick.appRoot.mmToPx(flick.controller.pageLayout.top)
                    hoverEnabled: true
                    cursorShape: Qt.IBeamCursor
                    onPressed: mouse => hotspot.pressAt(headerArea, mouse)
                    onPositionChanged: mouse => { if (pressed) hotspot.dragAt(headerArea, mouse) }
                    // Once the gesture is over: the area hides when editing starts.
                    onDoubleClicked: Qt.callLater(flick.appRoot.editPart, "header", hotspot.index + 1)
                }
                MouseArea {
                    id: footerArea
                    objectName: "footerArea"
                    y: parent.height - height
                    width: parent.width
                    height: flick.appRoot.mmToPx(flick.controller.pageLayout.bottom)
                    hoverEnabled: true
                    cursorShape: Qt.IBeamCursor
                    onPressed: mouse => hotspot.pressAt(footerArea, mouse)
                    onPositionChanged: mouse => { if (pressed) hotspot.dragAt(footerArea, mouse) }
                    // Once the gesture is over: the area hides when editing starts.
                    onDoubleClicked: Qt.callLater(flick.appRoot.editPart, "footer", hotspot.index + 1)
                }
            }
        }

        // While a header or footer is edited: the body dims, and a click
        // anywhere outside it goes back to the body.
        MouseArea {
            objectName: "partEditingShield"
            anchors.fill: parent
            visible: flick.appRoot.partEditing
            cursorShape: Qt.IBeamCursor
            onPressed: mouse => {
                const p = mapToItem(editor, mouse.x, mouse.y)
                flick.appRoot.finishPartEditing(editor.positionAt(p.x, p.y))
            }
        }

        // The header and footer editors: the TextEdits their documents
        // are installed into (DocumentController::setPartDocument()),
        // shown over the page being edited.
        TextEdit {
            id: headerEditor
            objectName: "headerEditor"
            visible: flick.appRoot.editingPart === "header"
            y: (flick.appRoot.editingPage - 1) * pageColumn.page.height + flick.controller.headerYPx
            x: flick.appRoot.mmToPx(flick.controller.pageLayout.left)
            width: pageColumn.width - x - flick.appRoot.mmToPx(flick.controller.pageLayout.right)
            height: implicitHeight
            padding: 0
            readOnly: flick.controller.loading || flick.controller.streamingEdit
            selectByMouse: true
            // Kept while the toolbar is used, which formats it.
            persistentSelection: true
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            color: flick.theme.foreground
            selectionColor: flick.theme.accent
            selectedTextColor: flick.theme.background
            onTextChanged: flick.edited()
            onCursorPositionChanged: if (visible) flick.appRoot.syncSelection()
            onSelectionStartChanged: if (visible) flick.appRoot.syncSelection()
            onSelectionEndChanged: if (visible) flick.appRoot.syncSelection()
            onCursorRectangleChanged: if (activeFocus) flick.ensureVisible(mapToItem(editor, cursorRectangle))
            Keys.onEscapePressed: flick.appRoot.finishPartEditing()
        }

        TextEdit {
            id: footerEditor
            objectName: "footerEditor"
            visible: flick.appRoot.editingPart === "footer"
            y: (flick.appRoot.editingPage - 1) * pageColumn.page.height + flick.controller.footerYPx
            x: flick.appRoot.mmToPx(flick.controller.pageLayout.left)
            width: pageColumn.width - x - flick.appRoot.mmToPx(flick.controller.pageLayout.right)
            height: implicitHeight
            padding: 0
            readOnly: flick.controller.loading || flick.controller.streamingEdit
            selectByMouse: true
            // Kept while the toolbar is used, which formats it.
            persistentSelection: true
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            color: flick.theme.foreground
            selectionColor: flick.theme.accent
            selectedTextColor: flick.theme.background
            onTextChanged: flick.edited()
            onCursorPositionChanged: if (visible) flick.appRoot.syncSelection()
            onSelectionStartChanged: if (visible) flick.appRoot.syncSelection()
            onSelectionEndChanged: if (visible) flick.appRoot.syncSelection()
            onCursorRectangleChanged: if (activeFocus) flick.ensureVisible(mapToItem(editor, cursorRectangle))
            Keys.onEscapePressed: flick.appRoot.finishPartEditing()
        }

        // Where the header ends / the footer begins, and the edit bar:
        // fields, and the way back to the body. The toolbar formats the
        // header or footer meanwhile.
        Rectangle {
            visible: flick.appRoot.partEditing
            x: 0
            width: pageColumn.width
            height: 1
            y: flick.appRoot.editingPart === "header"
               ? (flick.appRoot.editingPage - 1) * pageColumn.page.height
                 + Math.max(flick.appRoot.mmToPx(flick.controller.pageLayout.top), headerEditor.y + headerEditor.height
                                                                    - (flick.appRoot.editingPage - 1) * pageColumn.page.height)
               : flick.appRoot.editingPage * pageColumn.page.height
                 - Math.max(flick.appRoot.mmToPx(flick.controller.pageLayout.bottom), flick.appRoot.editingPage * pageColumn.page.height - footerEditor.y)
            color: flick.theme.accent
        }

        Rectangle {
            id: partBar
            objectName: "partBar"
            visible: flick.appRoot.partEditing
            readonly property real edge: flick.appRoot.editingPart === "header"
                ? Math.max((flick.appRoot.editingPage - 1) * pageColumn.page.height + flick.appRoot.mmToPx(flick.controller.pageLayout.top),
                           headerEditor.y + headerEditor.height)
                : Math.min(flick.appRoot.editingPage * pageColumn.page.height - flick.appRoot.mmToPx(flick.controller.pageLayout.bottom),
                           footerEditor.y)
            x: pageColumn.width - width - flick.appRoot.mmToPx(flick.controller.pageLayout.right)
            y: flick.appRoot.editingPart === "header" ? edge + 6 : edge - height - 6
            width: partBarRow.implicitWidth + 8
            height: partBarRow.implicitHeight + 8
            radius: 5
            color: flick.appRoot.barColor
            border.color: flick.theme.accent
            border.width: 1

            Row {
                id: partBarRow
                anchors.centerIn: parent
                spacing: 2
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    rightPadding: 6
                    leftPadding: 4
                    text: flick.appRoot.editingPart === "header" ? "Editing header" : "Editing footer"
                    color: flick.theme.accent
                    font.pixelSize: 12
                    font.bold: true
                }
                ToolButton {
                    objectName: "insertPageNumber"
                    appRoot: flick.appRoot
                    label: "# Page"
                    tip: "Insert the page number"
                    onClicked: flick.appRoot.insertPageField(false)
                }
                ToolButton {
                    objectName: "insertPageCount"
                    appRoot: flick.appRoot
                    label: "# Pages"
                    tip: "Insert the number of pages"
                    onClicked: flick.appRoot.insertPageField(true)
                }
                ToolButton {
                    objectName: "partDone"
                    appRoot: flick.appRoot
                    label: "Done"
                    tip: "Back to the document (Esc, or click the page)"
                    active: true
                    onClicked: flick.appRoot.finishPartEditing()
                }
            }
        }
    }
}
