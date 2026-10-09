import QtCore
import QtQuick
import QtQuick.Dialogs
import QtQuick.Window
import Rune

Window {
    id: root

    required property var theme

    readonly property color barColor: theme.lighter_background !== undefined ? theme.lighter_background : theme.background
    readonly property string documentName: controller.currentPath !== ""
        ? controller.currentPath.substring(controller.currentPath.lastIndexOf("/") + 1) : "Untitled"
    // Brief status-bar message ("Saved"); empty shows the document name.
    property string statusFlash: ""
    // Status-bar text while controller.busy ("Opening…" / "Saving…").
    property string busyText: ""
    // Runs once the user has saved or discarded unsaved changes; see
    // whenSafeToDiscard().
    property var pendingAction: null
    // Set once the user has agreed to lose unsaved changes on close.
    property bool closeConfirmed: false

    // Fixed presets the toolbar cycles/steps through.
    readonly property var fontNames: ["Sans", "Serif", "Monospace"]
    readonly property var fontSizes: [8, 10, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48]
    // Text color presets: [label, controller value]; "auto" is the default color.
    readonly property var textColors: [["Automatic", "auto"], ["Black", "#000000"], ["Red", "#cc0000"],
                                       ["Blue", "#0066cc"], ["Green", "#009933"]]

    // Toolbar state, refreshed from the controller on formatChanged.
    property bool boldActive: false
    property bool italicActive: false
    property bool underlineActive: false
    property bool bulletActive: false
    property bool numberedActive: false
    property int alignment: Qt.AlignLeft
    property string fontFamily: ""
    property real fontSize: 12
    property string textColor: "auto"
    property bool tableActive: false
    readonly property int colorIndex: textColors.findIndex(c => c[1] === textColor)

    function refreshFormat() {
        boldActive = controller.isBold()
        italicActive = controller.isItalic()
        underlineActive = controller.isUnderline()
        bulletActive = controller.isInBulletList()
        numberedActive = controller.isInNumberedList()
        alignment = controller.currentAlignment()
        fontFamily = controller.currentFontFamily()
        fontSize = controller.currentFontSize()
        textColor = controller.currentTextColor()
        tableActive = controller.isInTable()
    }

    // A family outside the presets (the default font) counts as the first.
    function cycleFont() {
        const i = Math.max(0, fontNames.indexOf(fontFamily))
        controller.setFontFamily(fontNames[(i + 1) % fontNames.length])
    }

    // Steps the font size to the next preset up (+1) or down (-1).
    function stepFontSize(direction) {
        const next = direction > 0 ? fontSizes.find(s => s > fontSize)
                                   : fontSizes.slice().reverse().find(s => s < fontSize)
        if (next !== undefined)
            controller.setFontSize(next)
    }

    function cycleColor() {
        controller.setTextColor(textColors[(colorIndex + 1) % textColors.length][1])
    }

    function syncSelection() {
        controller.setSelection(editor.cursorPosition, editor.selectionStart, editor.selectionEnd)
    }

    // File flow. Every open/save is asynchronous: results arrive in
    // onFileOpened / onFileSaved below. Saving writes .odt or .docx only, so
    // a document opened from a .doc (or not yet saved) goes through Save As.

    function suffixOf(path) {
        const name = path.substring(path.lastIndexOf("/") + 1)
        const dot = name.lastIndexOf(".")
        return dot > 0 ? name.substring(dot + 1).toLowerCase() : ""
    }

    function isSavable(path) {
        const suffix = suffixOf(path)
        return suffix === "odt" || suffix === "docx"
    }

    function folderOf(path) {
        return pathToUrl(path.substring(0, path.lastIndexOf("/")))
    }

    // Dialogs speak file:// URLs; the controller takes local paths.
    function urlToPath(url) {
        return decodeURIComponent(url.toString().replace(/^file:\/\//, ""))
    }

    function pathToUrl(path) {
        return "file://" + path.split("/").map(encodeURIComponent).join("/")
    }

    // Runs `action` now if nothing would be lost, else asks first: Save
    // runs it after a successful save, Discard runs it at once, Cancel
    // drops it.
    function whenSafeToDiscard(action) {
        if (controller.busy) return
        if (!controller.dirty) {
            action()
            return
        }
        pendingAction = action
        discardDialog.open()
    }

    function resolveDiscard(choice) {
        const action = pendingAction
        if (choice === "save") {
            save() // pendingAction runs from onFileSaved, or is dropped if Save As is cancelled
            return
        }
        pendingAction = null
        if (choice === "discard" && action)
            action()
    }

    function requestNew() {
        whenSafeToDiscard(() => {
            controller.newDocument()
            syncSelection()
        })
    }

    function requestOpen() {
        whenSafeToDiscard(() => {
            if (controller.currentPath !== "")
                openDialog.currentFolder = folderOf(controller.currentPath)
            openDialog.open()
        })
    }

    function openPath(path) {
        if (controller.busy) return
        busyText = "Opening…"
        controller.openFile(path)
    }

    function save() {
        if (controller.busy) return
        if (isSavable(controller.currentPath))
            saveTo(controller.currentPath)
        else
            saveAs()
    }

    function saveAs() {
        if (controller.busy) return
        const current = controller.currentPath
        if (current !== "") {
            // Suggest the current name, as .odt unless it already saves as .docx.
            const base = current.substring(0, current.length - suffixOf(current).length - 1)
            const docx = suffixOf(current) === "docx"
            saveDialog.selectedNameFilter.index = docx ? 1 : 0
            saveDialog.currentFolder = folderOf(current)
            saveDialog.selectedFile = pathToUrl(base + (docx ? ".docx" : ".odt"))
        }
        saveDialog.open()
    }

    // A name typed without a usable extension gets the chosen filter's.
    function acceptSave(path) {
        if (!isSavable(path))
            path += "." + saveDialog.selectedNameFilter.extensions[0]
        saveTo(path)
    }

    function saveTo(path) {
        busyText = "Saving…"
        controller.saveFile(path)
    }

    width: 1000
    height: 800
    visible: true
    color: theme.background
    title: documentName + (controller.dirty ? " ●" : "") + " — Rune"

    onClosing: close => {
        if (closeConfirmed || !controller.dirty)
            return
        close.accepted = false
        whenSafeToDiscard(() => {
            closeConfirmed = true
            root.close()
        })
    }

    // Installs the controller's document into the TextEdit; see
    // DocumentController.h for why this isn't a binding.
    Component.onCompleted: {
        controller.document = editor.textDocument
        controller.newDocument()
        syncSelection()
    }

    DocumentController {
        id: controller
        objectName: "controller"
        onFormatChanged: root.refreshFormat()
        onCursorPositionRequested: position => editor.cursorPosition = position
    }

    Connections {
        target: controller
        function onFileSaved(success, path, error) {
            root.statusFlash = success ? "Saved " + path : "Save failed: " + error
            flashTimer.restart()
            const action = root.pendingAction
            root.pendingAction = null
            if (success && action)
                action()
        }
        function onFileOpened(success, path, error) {
            root.statusFlash = success ? "Opened " + path : "Open failed: " + error
            flashTimer.restart()
            if (success)
                root.syncSelection()
        }
    }

    FileDialog {
        id: openDialog
        objectName: "openDialog"
        title: "Open"
        fileMode: FileDialog.OpenFile
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        nameFilters: ["Documents (*.odt *.docx *.doc)", "OpenDocument Text (*.odt)",
                      "Word Document (*.docx *.doc)", "All files (*)"]
        onAccepted: root.openPath(root.urlToPath(selectedFile))
    }

    FileDialog {
        id: saveDialog
        objectName: "saveDialog"
        title: "Save As"
        fileMode: FileDialog.SaveFile
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        // Order matters: saveAs() and acceptSave() index into these.
        nameFilters: ["OpenDocument Text (*.odt)", "Word Document (*.docx)"]
        defaultSuffix: selectedNameFilter.extensions[0]
        onAccepted: root.acceptSave(root.urlToPath(selectedFile))
        onRejected: root.pendingAction = null
    }

    MessageDialog {
        id: discardDialog
        objectName: "discardDialog"
        title: "Unsaved changes"
        text: "Save changes to " + root.documentName + "?"
        informativeText: "Your changes will be lost if you don't save them."
        buttons: MessageDialog.Save | MessageDialog.Discard | MessageDialog.Cancel
        onButtonClicked: (button, role) => root.resolveDiscard(
            button === MessageDialog.Save ? "save" : button === MessageDialog.Discard ? "discard" : "cancel")
    }

    Shortcut { sequence: StandardKey.New; onActivated: root.requestNew() }
    Shortcut { sequence: StandardKey.Open; onActivated: root.requestOpen() }
    Shortcut { sequence: StandardKey.Save; onActivated: root.save() }
    Shortcut { sequence: StandardKey.SaveAs; onActivated: root.saveAs() }
    // While the editor has focus it takes these keys itself (see its
    // Keys.onPressed); these cover focus anywhere else in the window.
    Shortcut { sequence: StandardKey.Undo; onActivated: controller.undo() }
    Shortcut { sequence: StandardKey.Redo; onActivated: controller.redo() }
    Shortcut { sequence: StandardKey.Bold; onActivated: controller.toggleBold() }
    Shortcut { sequence: StandardKey.Italic; onActivated: controller.toggleItalic() }
    Shortcut { sequence: StandardKey.Underline; onActivated: controller.toggleUnderline() }

    component ToolSeparator: Rectangle {
        width: 1
        height: 20
        anchors.verticalCenter: parent.verticalCenter
        color: root.theme.muted
    }

    Rectangle {
        id: toolbar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 40
        z: 2 // tooltips overlap the document view
        color: root.barColor

        Row {
            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 4 }
            spacing: 4

            ToolButton {
                objectName: "openButton"
                appRoot: root
                label: "Open"; tip: "Open (Ctrl+O)"
                available: !controller.busy
                onClicked: root.requestOpen()
            }
            ToolButton {
                objectName: "saveButton"
                appRoot: root
                label: "Save"; tip: "Save (Ctrl+S) · Save As (Ctrl+Shift+S)"
                available: !controller.busy
                onClicked: root.save()
            }

            ToolSeparator {}

            ToolButton {
                objectName: "undoButton"
                appRoot: root
                label: "↶"; tip: "Undo (Ctrl+Z)"
                available: controller.canUndo && !controller.busy
                onClicked: controller.undo()
            }
            ToolButton {
                objectName: "redoButton"
                appRoot: root
                label: "↷"; tip: "Redo (Ctrl+Shift+Z)"
                available: controller.canRedo && !controller.busy
                onClicked: controller.redo()
            }

            ToolSeparator {}

            ToolButton {
                objectName: "boldButton"
                appRoot: root
                label: "B"; bold: true; tip: "Bold (Ctrl+B)"
                active: root.boldActive
                onClicked: controller.toggleBold()
            }
            ToolButton {
                appRoot: root
                label: "I"; italic: true; tip: "Italic (Ctrl+I)"
                active: root.italicActive
                onClicked: controller.toggleItalic()
            }
            ToolButton {
                appRoot: root
                label: "U"; underline: true; tip: "Underline (Ctrl+U)"
                active: root.underlineActive
                onClicked: controller.toggleUnderline()
            }

            ToolSeparator {}

            ToolButton {
                objectName: "bulletButton"
                appRoot: root
                label: "•"; tip: "Bullet list (Tab / Shift+Tab to nest)"
                active: root.bulletActive
                onClicked: controller.toggleBulletList()
            }
            ToolButton {
                objectName: "numberedButton"
                appRoot: root
                label: "1."; tip: "Numbered list (Tab / Shift+Tab to nest)"
                active: root.numberedActive
                onClicked: controller.toggleNumberedList()
            }

            ToolSeparator {}

            Repeater {
                model: [["L", Qt.AlignLeft, "Align left", "alignLeftButton"],
                        ["C", Qt.AlignHCenter, "Align center", "alignCenterButton"],
                        ["R", Qt.AlignRight, "Align right", "alignRightButton"]]
                ToolButton {
                    required property var modelData
                    objectName: modelData[3]
                    appRoot: root
                    label: modelData[0]
                    tip: modelData[2]
                    active: root.alignment === modelData[1]
                    onClicked: controller.setAlignment(modelData[1])
                }
            }

            ToolSeparator {}

            ToolButton {
                objectName: "fontButton"
                appRoot: root
                label: root.fontFamily + " ▾"
                tip: "Font: click for next (" + root.fontNames.join(", ") + ")"
                onClicked: root.cycleFont()
            }
            ToolButton {
                objectName: "fontSmallerButton"
                appRoot: root
                label: "−"; tip: "Decrease font size"
                onClicked: root.stepFontSize(-1)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: 40
                horizontalAlignment: Text.AlignHCenter
                text: root.fontSize + "pt"
                color: root.theme.foreground
                font.pixelSize: 13
            }
            ToolButton {
                objectName: "fontLargerButton"
                appRoot: root
                label: "+"; tip: "Increase font size"
                onClicked: root.stepFontSize(1)
            }

            ToolSeparator {}

            ToolButton {
                id: colorButton
                objectName: "colorButton"
                appRoot: root
                label: "A"; bold: true
                tip: "Text color: " + (root.colorIndex >= 0 ? root.textColors[root.colorIndex][0] : "Custom")
                     + " (click for next)"
                active: root.textColor !== "auto"
                onClicked: root.cycleColor()

                // Swatch of the color at the cursor; automatic shows as the foreground.
                Rectangle {
                    anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 4 }
                    width: 16; height: 3; radius: 1
                    color: root.textColor === "auto" ? root.theme.foreground : root.textColor
                }
            }

            ToolSeparator {}

            ToolButton {
                objectName: "tableButton"
                appRoot: root
                label: "⊞ Table"; tip: "Insert 3×3 table"
                onClicked: controller.insertTable(3, 3)
            }
            ToolButton {
                objectName: "insertRowButton"
                appRoot: root
                label: "+Row"; tip: "Insert row below"
                available: root.tableActive
                onClicked: controller.insertTableRow()
            }
            ToolButton {
                objectName: "deleteRowButton"
                appRoot: root
                label: "−Row"; tip: "Delete row"
                available: root.tableActive
                onClicked: controller.deleteTableRow()
            }
            ToolButton {
                objectName: "insertColumnButton"
                appRoot: root
                label: "+Col"; tip: "Insert column to the right"
                available: root.tableActive
                onClicked: controller.insertTableColumn()
            }
            ToolButton {
                objectName: "deleteColumnButton"
                appRoot: root
                label: "−Col"; tip: "Delete column"
                available: root.tableActive
                onClicked: controller.deleteTableColumn()
            }
        }

        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1
            color: Qt.alpha(root.theme.muted, 0.5)
        }
    }

    Flickable {
        id: flick
        anchors { left: parent.left; right: parent.right; top: toolbar.bottom; bottom: statusBar.top }
        contentWidth: width
        contentHeight: editor.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        // Keep the cursor in view while typing.
        function ensureVisible(r) {
            if (contentY >= r.y)
                contentY = r.y
            else if (contentY + height <= r.y + r.height)
                contentY = r.y + r.height - height
        }

        TextEdit {
            id: editor
            objectName: "editor"
            width: flick.width
            height: Math.max(implicitHeight, flick.height)
            padding: 48
            focus: true
            // A .docx open replaces the document when it finishes; edits
            // made meanwhile would be lost (see DocumentController::openFile).
            readOnly: controller.busy
            selectByMouse: true
            persistentSelection: true
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            color: root.theme.foreground
            selectionColor: root.theme.accent
            selectedTextColor: root.theme.background
            font.pointSize: 12

            onCursorPositionChanged: root.syncSelection()
            onSelectionStartChanged: root.syncSelection()
            onSelectionEndChanged: root.syncSelection()
            onCursorRectangleChanged: flick.ensureVisible(cursorRectangle)

            // Undo/redo go through the controller rather than the TextEdit's
            // built-in handling, which knows nothing of pending formats or
            // the toolbar. Printable text typed with a pending format (e.g.
            // Bold toggled with nothing selected) is inserted by the
            // controller, so it is one undo step; see typeWithPendingFormat().
            Keys.onPressed: event => {
                if (event.matches(StandardKey.Undo))
                    controller.undo()
                else if (event.matches(StandardKey.Redo))
                    controller.redo()
                else if (!(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                         && event.text.length > 0 && event.text >= " " && event.text !== "\x7f"
                         && controller.typeWithPendingFormat(event.text))
                    ; // inserted
                else
                    return
                event.accepted = true
            }

            // Tab / Shift+Tab move between table cells, or else nest list
            // items; elsewhere Tab is left to the TextEdit, which inserts a
            // tab character.
            Keys.onTabPressed: event => {
                if (controller.isInTable())
                    controller.nextTableCell()
                else if (controller.isInBulletList() || controller.isInNumberedList())
                    controller.demoteListItem()
                else
                    event.accepted = false
            }
            Keys.onBacktabPressed: event => {
                if (controller.isInTable())
                    controller.previousTableCell()
                else if (controller.isInBulletList() || controller.isInNumberedList())
                    controller.promoteListItem()
                else
                    event.accepted = false
            }
        }
    }

    Rectangle {
        id: statusBar
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 28
        color: root.barColor

        Rectangle {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 1
            color: root.theme.accent
        }

        Text {
            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 10
                      right: parent.right; rightMargin: 10 }
            color: root.theme.foreground
            font.family: "monospace"
            font.pixelSize: 12
            elide: Text.ElideRight
            text: controller.busy ? root.busyText
                  : root.statusFlash !== "" ? root.statusFlash
                  : root.documentName + (controller.dirty ? " ●" : "")
        }

        Timer { id: flashTimer; interval: 2500; onTriggered: root.statusFlash = "" }
    }
}
