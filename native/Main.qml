import QtCore
import QtQuick
import QtQuick.Dialogs
import QtQuick.Window
import Rune

Window {
    id: root

    required property var theme

    // A page: a shade lighter than the window behind it.
    readonly property color pageColor: Qt.tint(theme.background, Qt.alpha(theme.foreground, 0.07))
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

    // Autosave (File → Autosave, on by default): an edited document with a
    // file Save can write to saves itself once typing pauses, and when the
    // window loses focus. Untitled documents and .doc files (Save writes
    // .odt/.docx only) still need Save. .docx waits longer: each save is a
    // LibreOffice conversion.
    property alias autosaveEnabled: settings.autosave
    readonly property bool autosaveActive: settings.autosave && isSavable(controller.currentPath)
    readonly property int autosaveDelay: suffixOf(controller.currentPath) === "docx" ? 30000 : 5000
    // A save is in flight (one call to saveTo(), until onFileSaved), and
    // whether autosave started it.
    property bool saving: false
    property bool autosaving: false
    // The last save to the current document failed, and why; the next
    // successful save clears it.
    property bool saveFailed: false
    property string saveError: ""

    // Toolbar groups that move into the ⋯ menu when the window is too
    // narrow, first to go first. The rest always stay on the toolbar.
    readonly property var collapseOrder: ["table", "color", "align", "lists", "history"]
    // The groups in the ⋯ menu now: as many from collapseOrder as it takes
    // for the toolbar (with the ⋯ button) to fit left of the AI button.
    readonly property var collapsedGroups: {
        const groups = {history: historyGroup, lists: listsGroup, align: alignGroup, color: colorGroup, table: tableGroup}
        const gap = toolRow.spacing
        const room = toolbar.width - toolRow.anchors.leftMargin - aiButton.width - aiButton.anchors.rightMargin - gap
        let used = fileGroup.width + styleGroup.width + fontGroup.width + 2 * gap
        for (const name of collapseOrder)
            used += groups[name].width + gap
        const collapsed = []
        for (const name of collapseOrder) {
            if (used <= room)
                break
            if (collapsed.length === 0)
                used += overflowWidth + gap
            used -= groups[name].width + gap
            collapsed.push(name)
        }
        return collapsed
    }
    // The ⋯ button and the separator before it.
    readonly property real overflowWidth: overflowMenu.width + 1 + toolRow.spacing

    // The header or footer being edited in place, on which page: "" (the
    // body), "header" or "footer"; editingPage counts from 1.
    property string editingPart: ""
    property int editingPage: 1
    readonly property bool partEditing: editingPart !== ""

    // Millimetres (the page layout's unit) in the editor's pixels.
    function mmToPx(mm) {
        return mm * 96 / 25.4
    }

    // File → Page setup….
    function openPageSetup() {
        pageSetup.open()
    }

    // Starts editing the header or footer (`part`) where it is on `page`,
    // the cursor at its end.
    function editPart(part, page) {
        // Straight from the other one: it may need room too.
        if (partEditing)
            controller.fitPageParts()
        // The toolbar now formats the header or footer.
        controller.setEditTarget(part === "footer" ? DocumentController.Footer : DocumentController.Header)
        editingPart = part
        editingPage = Math.max(1, Math.min(page, controller.pageCount))
        controller.setPartEditing(editingPage, theme.foreground)
        const partEditor = activePartEditor()
        partEditor.forceActiveFocus()
        partEditor.cursorPosition = partEditor.length
        syncSelection()
        flick.ensureVisible(partEditor.mapToItem(editor, partEditor.cursorRectangle))
    }

    // Back to the body, its cursor where it was or, with `position`, there.
    // A header or footer grown past its margin gets the room it needs (see
    // DocumentController::fitPageParts()).
    function finishPartEditing(position) {
        if (!partEditing)
            return
        editingPart = ""
        controller.setEditTarget(DocumentController.Body)
        controller.fitPageParts()
        editor.forceActiveFocus()
        if (position !== undefined)
            editor.cursorPosition = position
        syncSelection()
    }

    // The TextEdit the selection and toolbar follow: the body's, or the
    // header's or footer's while one is edited.
    function activeEditor() {
        return partEditing ? activePartEditor() : editor
    }

    function activePartEditor() {
        return editingPart === "footer" ? footerEditor : headerEditor
    }

    // Edit bar: a page number or page count field at the cursor.
    function insertPageField(count) {
        const partEditor = activePartEditor()
        if (partEditor.selectedText !== "")
            partEditor.remove(partEditor.selectionStart, partEditor.selectionEnd)
        partEditor.cursorPosition = controller.insertPageField(editingPart === "footer", partEditor.cursorPosition, count)
    }

    // Ctrl+K → Page numbers: a centred page number in the footer if it is
    // empty; either way, the footer opens for editing.
    function addPageNumbers() {
        const empty = !controller.hasFooter
        editPart("footer", 1)
        if (empty) {
            controller.setAlignment(Qt.AlignHCenter)
            insertPageField(false)
        }
    }

    // Ctrl+K, and File → Command palette….
    function openCommandPalette() {
        commandPalette.open()
    }

    // Everything the command palette (Ctrl+K) offers, in the order it lists
    // them when nothing is typed. `available` says whether a command applies
    // right now; hints match the shortcuts above.
    readonly property var commands: {
        const always = () => true
        const idle = () => !controller.busy
        // Lists, tables and the AI are the body's only, not a header's or footer's.
        const body = () => !root.partEditing
        const inTable = () => !root.partEditing && controller.isInTable()
        const align = (name, value) => ({group: "Format", name: name, available: always,
                                         run: () => controller.setAlignment(value)})
        return [
            {group: "File", name: "New", hint: "Ctrl+N", available: idle, run: () => root.requestNew()},
            {group: "File", name: "Open…", hint: "Ctrl+O", available: idle, run: () => root.requestOpen()},
            {group: "File", name: "Save", hint: "Ctrl+S", available: idle, run: () => root.save()},
            {group: "File", name: "Save As…", hint: "Ctrl+Shift+S", available: idle, run: () => root.saveAs()},
            {group: "File", name: "Page setup…", available: always, run: () => root.openPageSetup()},
            {group: "Page", name: "Edit header", available: always, run: () => root.editPart("header", root.currentPage())},
            {group: "Page", name: "Edit footer", available: always, run: () => root.editPart("footer", root.currentPage())},
            {group: "Page", name: "Page numbers", available: always, run: () => root.addPageNumbers()},
            {group: "File", name: settings.autosave ? "Turn autosave off" : "Turn autosave on", available: always,
             run: () => settings.autosave = !settings.autosave},
            {group: "Edit", name: "Undo", hint: "Ctrl+Z", available: () => controller.canUndo && !controller.loading,
             run: () => controller.undo()},
            {group: "Edit", name: "Redo", hint: "Ctrl+Shift+Z", available: () => controller.canRedo && !controller.loading,
             run: () => controller.redo()},
            {group: "Format", name: "Bold", hint: "Ctrl+B", available: always, run: () => controller.toggleBold()},
            {group: "Format", name: "Italic", hint: "Ctrl+I", available: always, run: () => controller.toggleItalic()},
            {group: "Format", name: "Underline", hint: "Ctrl+U", available: always, run: () => controller.toggleUnderline()},
            {group: "Format", name: "Font…", available: always, run: () => fontPicker.openList()},
            {group: "Format", name: "Increase font size", available: always, run: () => root.stepFontSize(1)},
            {group: "Format", name: "Decrease font size", available: always, run: () => root.stepFontSize(-1)},
            {group: "Format", name: "Text color…", available: always,
             run: () => colorPicker.openPalette(colorGroup.visible ? colorPicker : overflowMenu)},
            {group: "Format", name: "Remove text color", available: () => root.textColor !== "auto",
             run: () => controller.setTextColor("auto")},
            align("Align left", Qt.AlignLeft),
            align("Align center", Qt.AlignHCenter),
            align("Align right", Qt.AlignRight),
            {group: "List", name: "Bullet list", available: body, run: () => controller.toggleBulletList()},
            {group: "List", name: "Numbered list", available: body, run: () => controller.toggleNumberedList()},
            {group: "Table", name: "Insert table", hint: "3×3", available: body, run: () => controller.insertTable(3, 3)},
            {group: "Table", name: "Insert row above", available: inTable, run: () => controller.insertTableRowAbove()},
            {group: "Table", name: "Insert row below", available: inTable, run: () => controller.insertTableRow()},
            {group: "Table", name: "Delete row", available: inTable, run: () => controller.deleteTableRow()},
            {group: "Table", name: "Insert column left", available: inTable, run: () => controller.insertTableColumnLeft()},
            {group: "Table", name: "Insert column right", available: inTable, run: () => controller.insertTableColumn()},
            {group: "Table", name: "Delete column", available: inTable, run: () => controller.deleteTableColumn()},
            {group: "Table", name: "Delete table", available: inTable, run: () => controller.deleteTable()},
            {group: "AI", name: "Edit with AI", hint: "Ctrl+Shift+E", available: body, run: () => aiPanel.startEdit()},
            {group: "AI", name: aiPanel.visible ? "Hide AI panel" : "Show AI panel", available: always,
             run: () => aiPanel.visible = !aiPanel.visible}
        ]
    }

    // Font sizes the toolbar's − / + step through.
    readonly property var fontSizes: [8, 10, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48]

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

    // The page the body's cursor is on, from 1.
    function currentPage() {
        return Math.min(controller.pageCount, Math.floor(editor.cursorRectangle.y / controller.pageSizePx.height) + 1)
    }

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

    // Steps the font size to the next preset up (+1) or down (-1).
    function stepFontSize(direction) {
        const next = direction > 0 ? fontSizes.find(s => s > fontSize)
                                   : fontSizes.slice().reverse().find(s => s < fontSize)
        if (next !== undefined)
            controller.setFontSize(next)
    }

    function syncSelection() {
        const active = activeEditor()
        controller.setSelection(active.cursorPosition, active.selectionStart, active.selectionEnd)
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
            editingPart = ""
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
        finishPartEditing()
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

    // After a file dialog closes. Qt's own (non-native) file dialog is a
    // popup window, and Qt 6.8-6.10 leave keyboard focus with it once
    // hidden: typing went nowhere until the document was clicked.
    function refocusEditor() {
        root.requestActivate()
        editor.forceActiveFocus()
    }

    function saveTo(path) {
        busyText = "Saving…"
        saving = true
        controller.saveFile(path)
    }

    // Saves the current document now if autosave applies and it has
    // unsaved changes; otherwise waits for the next pause.
    function autosaveNow() {
        autosaveTimer.stop()
        if (!autosaveActive || !controller.dirty || controller.streamingEdit)
            return
        if (controller.busy) {
            autosaveTimer.restart() // try again after this operation
            return
        }
        autosaving = true
        saveTo(controller.currentPath)
    }

    onActiveChanged: if (!active) autosaveNow()
    onAutosaveActiveChanged: {
        if (!autosaveActive)
            autosaveTimer.stop()
        else if (controller.dirty)
            autosaveTimer.restart()
    }

    width: 1000
    // File, Save, B/I/U, the font controls, ⋯ and AI: what never collapses.
    minimumWidth: fileGroup.width + styleGroup.width + fontGroup.width + overflowWidth + aiButton.width + 30
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
        controller.setPartDocument(false, headerEditor.textDocument)
        controller.setPartDocument(true, footerEditor.textDocument)
        controller.newDocument()
        syncSelection()
    }

    AiManager {
        id: ai
        objectName: "ai"
        document: controller
    }

    DocumentController {
        id: controller
        objectName: "controller"
        onFormatChanged: root.refreshFormat()
        onCursorPositionRequested: position => root.activeEditor().cursorPosition = position
    }

    Settings {
        id: settings
        category: "editor"
        property bool autosave: true
    }

    // Restarted by every edit, so it fires once typing pauses.
    Timer {
        id: autosaveTimer
        objectName: "autosaveTimer"
        interval: root.autosaveDelay
        onTriggered: root.autosaveNow()
    }

    Connections {
        target: controller
        function onCurrentPathChanged() {
            root.saveFailed = false
            root.saveError = ""
        }
        function onStreamingEditChanged() {
            if (!controller.streamingEdit && root.autosaveActive && controller.dirty)
                autosaveTimer.restart()
        }
        function onFileSaved(success, path, error) {
            const wasAuto = root.autosaving
            root.saving = false
            root.autosaving = false
            if (success || path === controller.currentPath) {
                root.saveFailed = !success
                root.saveError = error
            }
            // A successful autosave shows on the Save button, not here.
            if (!(wasAuto && success)) {
                root.statusFlash = success ? "Saved " + path : (wasAuto ? "Autosave failed: " : "Save failed: ") + error
                flashTimer.restart()
            }
            const action = root.pendingAction
            root.pendingAction = null
            if (success && action)
                action()
        }
        function onFileOpened(success, path, error) {
            if (success)
                root.editingPart = "" // the header and footer were replaced
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
        onVisibleChanged: if (!visible) root.refocusEditor()
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
        onVisibleChanged: if (!visible) root.refocusEditor()
    }

    UnsavedChangesDialog {
        id: discardDialog
        objectName: "discardDialog"
        title: "Unsaved changes"
        text: "Save changes to " + root.documentName + "?"
        informativeText: "Your changes will be lost if you don't save them."
        onChosen: choice => root.resolveDiscard(choice)
    }

    Shortcut { sequences: [StandardKey.New]; onActivated: root.requestNew() }
    Shortcut { sequences: [StandardKey.Open]; onActivated: root.requestOpen() }
    Shortcut { sequences: [StandardKey.Save]; onActivated: root.save() }
    Shortcut { sequences: [StandardKey.SaveAs]; onActivated: root.saveAs() }
    Shortcut { sequences: ["Alt+F"]; onActivated: fileMenu.openMenu() }
    Shortcut { sequences: ["Ctrl+K"]; onActivated: root.openCommandPalette() }
    // While the editor has focus it takes these keys itself (see its
    // Keys.onPressed); these cover focus anywhere else in the window. While
    // a header or footer is edited, its TextEdit keeps Ctrl+Z / Ctrl+Shift+Z
    // for its own history, and the AI stays with the body.
    Shortcut { sequences: [StandardKey.Undo]; onActivated: controller.undo(); enabled: !root.partEditing }
    Shortcut { sequences: [StandardKey.Redo]; onActivated: controller.redo(); enabled: !root.partEditing }
    // AI: open the panel ready to edit the document (selection or cursor).
    Shortcut { sequences: ["Ctrl+Shift+E"]; onActivated: aiPanel.startEdit(); enabled: !root.partEditing }
    Shortcut { sequences: [StandardKey.Bold]; onActivated: controller.toggleBold() }
    Shortcut { sequences: [StandardKey.Italic]; onActivated: controller.toggleItalic() }
    Shortcut { sequences: [StandardKey.Underline]; onActivated: controller.toggleUnderline() }

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

        // The toolbar, in groups. When the window is too narrow for all of
        // them, the lowest-priority groups (root.collapseOrder) move into
        // the ⋯ menu instead of running under the AI button; File, Save,
        // B/I/U and the font controls always stay.
        Row {
            id: toolRow
            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 4 }
            spacing: 4

            Row {
                id: fileGroup
                spacing: 4
                FileMenu {
                    id: fileMenu
                    objectName: "fileMenuButton"
                    appRoot: root
                    available: !controller.busy
                }
                // Shows where the document stands: "✓ Saved" (dimmed) once
                // autosave has written it, "Saving…", or "Save failed" (click
                // to retry); a plain "Save" when it has unsaved changes or
                // autosave doesn't apply.
                ToolButton {
                    objectName: "saveButton"
                    appRoot: root
                    width: 88 // the widest label, so the toolbar doesn't shift
                    label: root.saving && controller.busy ? "Saving…"
                         : root.saveFailed ? "Save failed"
                         : root.autosaveActive && !controller.dirty ? "✓ Saved"
                         : "Save"
                    labelColor: root.saveFailed ? root.theme.red
                              : label === "Save" ? root.theme.foreground
                              : Qt.alpha(root.theme.foreground, 0.55)
                    tip: root.saveFailed ? root.saveError + " · click to retry"
                         : root.autosaveActive ? "Autosave is on · Save now (Ctrl+S)"
                         : "Save (Ctrl+S)"
                    available: !controller.busy
                    onClicked: root.save()
                }
            }

            Row {
                id: historyGroup
                objectName: "historyGroup"
                spacing: 4
                visible: !root.collapsedGroups.includes("history")
                ToolSeparator {}
                ToolButton {
                    objectName: "undoButton"
                    appRoot: root
                    label: "↶"; tip: "Undo (Ctrl+Z)"
                    available: controller.canUndo && !controller.loading
                    onClicked: controller.undo()
                }
                ToolButton {
                    objectName: "redoButton"
                    appRoot: root
                    label: "↷"; tip: "Redo (Ctrl+Shift+Z)"
                    available: controller.canRedo && !controller.loading
                    onClicked: controller.redo()
                }
            }

            Row {
                id: styleGroup
                spacing: 4
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
            }

            Row {
                id: listsGroup
                enabled: !root.partEditing
                opacity: enabled ? 1 : 0.4
                objectName: "listsGroup"
                spacing: 4
                visible: !root.collapsedGroups.includes("lists")
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
            }

            Row {
                id: alignGroup
                objectName: "alignGroup"
                spacing: 4
                visible: !root.collapsedGroups.includes("align")
                ToolSeparator {}
                // Shows the alignment at the cursor; the menu sets it.
                ToolMenu {
                    objectName: "alignButton"
                    menuName: "alignMenu"
                    appRoot: root
                    icon: AlignIcon { alignment: root.alignment; color: root.theme.foreground }
                    onClosed: editor.forceActiveFocus()
                    MenuEntry {
                        objectName: "alignLeftItem"
                        text: "Align left"; hint: root.alignment === Qt.AlignLeft ? "✓" : ""
                        onTriggered: controller.setAlignment(Qt.AlignLeft)
                    }
                    MenuEntry {
                        objectName: "alignCenterItem"
                        text: "Align center"; hint: root.alignment === Qt.AlignHCenter ? "✓" : ""
                        onTriggered: controller.setAlignment(Qt.AlignHCenter)
                    }
                    MenuEntry {
                        objectName: "alignRightItem"
                        text: "Align right"; hint: root.alignment === Qt.AlignRight ? "✓" : ""
                        onTriggered: controller.setAlignment(Qt.AlignRight)
                    }
                }
            }

            Row {
                id: fontGroup
                spacing: 4
                ToolSeparator {}
                FontPicker {
                    id: fontPicker
                    objectName: "fontButton"
                    anchors.verticalCenter: parent.verticalCenter
                    appRoot: root
                    controller: controller
                    currentFamily: root.fontFamily
                    onClosed: editor.forceActiveFocus()
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
            }

            Row {
                id: colorGroup
                objectName: "colorGroup"
                spacing: 4
                visible: !root.collapsedGroups.includes("color")
                ToolSeparator {}
                ColorPicker {
                    id: colorPicker
                    objectName: "colorButton"
                    anchors.verticalCenter: parent.verticalCenter
                    appRoot: root
                    controller: controller
                    currentColor: root.textColor
                    onClosed: editor.forceActiveFocus()
                }
            }

            Row {
                id: tableGroup
                enabled: !root.partEditing
                opacity: enabled ? 1 : 0.4
                objectName: "tableGroup"
                spacing: 4
                visible: !root.collapsedGroups.includes("table")
                ToolSeparator {}
                ToolMenu {
                    objectName: "tableButton"
                    menuName: "tableMenu"
                    appRoot: root
                    label: "⊞ Table"
                    onClosed: editor.forceActiveFocus()
                    MenuEntry {
                        objectName: "insertTableItem"
                        text: "Insert table"; hint: "3×3"
                        onTriggered: controller.insertTable(3, 3)
                    }
                    MenuDivider { theme: root.theme }
                    MenuEntry {
                        objectName: "insertRowAboveItem"
                        text: "Insert row above"
                        enabled: root.tableActive
                        onTriggered: controller.insertTableRowAbove()
                    }
                    MenuEntry {
                        objectName: "insertRowItem"
                        text: "Insert row below"
                        enabled: root.tableActive
                        onTriggered: controller.insertTableRow()
                    }
                    MenuEntry {
                        objectName: "deleteRowItem"
                        text: "Delete row"
                        enabled: root.tableActive
                        onTriggered: controller.deleteTableRow()
                    }
                    MenuDivider { theme: root.theme }
                    MenuEntry {
                        objectName: "insertColumnLeftItem"
                        text: "Insert column left"
                        enabled: root.tableActive
                        onTriggered: controller.insertTableColumnLeft()
                    }
                    MenuEntry {
                        objectName: "insertColumnItem"
                        text: "Insert column right"
                        enabled: root.tableActive
                        onTriggered: controller.insertTableColumn()
                    }
                    MenuEntry {
                        objectName: "deleteColumnItem"
                        text: "Delete column"
                        enabled: root.tableActive
                        onTriggered: controller.deleteTableColumn()
                    }
                    MenuDivider { theme: root.theme }
                    MenuEntry {
                        objectName: "deleteTableItem"
                        text: "Delete table"
                        enabled: root.tableActive
                        onTriggered: controller.deleteTable()
                    }
                }
            }

            // The groups that didn't fit, as menu entries, in toolbar order.
            Row {
                spacing: 4
                visible: root.collapsedGroups.length > 0
                ToolSeparator {}
                ToolMenu {
                    id: overflowMenu
                    objectName: "overflowButton"
                    menuName: "overflowMenu"
                    appRoot: root
                    label: "⋯"
                    caret: false
                    onClosed: if (!colorPicker.paletteOpen) editor.forceActiveFocus()

                    MenuEntry {
                        objectName: "overflowUndo"
                        visible: root.collapsedGroups.includes("history")
                        text: "Undo"; hint: "Ctrl+Z"
                        enabled: controller.canUndo && !controller.loading
                        onTriggered: controller.undo()
                    }
                    MenuEntry {
                        objectName: "overflowRedo"
                        visible: root.collapsedGroups.includes("history")
                        text: "Redo"; hint: "Ctrl+Shift+Z"
                        enabled: controller.canRedo && !controller.loading
                        onTriggered: controller.redo()
                    }
                    MenuDivider {
                        theme: root.theme
                        visible: root.collapsedGroups.includes("lists") && root.collapsedGroups.includes("history")
                    }
                    MenuEntry {
                        objectName: "overflowBullet"
                        visible: root.collapsedGroups.includes("lists")
                        text: "Bullet list"; hint: root.bulletActive ? "✓" : ""
                        onTriggered: controller.toggleBulletList()
                    }
                    MenuEntry {
                        objectName: "overflowNumbered"
                        visible: root.collapsedGroups.includes("lists")
                        text: "Numbered list"; hint: root.numberedActive ? "✓" : ""
                        onTriggered: controller.toggleNumberedList()
                    }
                    MenuDivider {
                        theme: root.theme
                        visible: root.collapsedGroups.includes("align")
                                 && (root.collapsedGroups.includes("history") || root.collapsedGroups.includes("lists"))
                    }
                    MenuEntry {
                        objectName: "overflowAlignLeft"
                        visible: root.collapsedGroups.includes("align")
                        text: "Align left"; hint: root.alignment === Qt.AlignLeft ? "✓" : ""
                        onTriggered: controller.setAlignment(Qt.AlignLeft)
                    }
                    MenuEntry {
                        objectName: "overflowAlignCenter"
                        visible: root.collapsedGroups.includes("align")
                        text: "Align center"; hint: root.alignment === Qt.AlignHCenter ? "✓" : ""
                        onTriggered: controller.setAlignment(Qt.AlignHCenter)
                    }
                    MenuEntry {
                        objectName: "overflowAlignRight"
                        visible: root.collapsedGroups.includes("align")
                        text: "Align right"; hint: root.alignment === Qt.AlignRight ? "✓" : ""
                        onTriggered: controller.setAlignment(Qt.AlignRight)
                    }
                    MenuDivider {
                        theme: root.theme
                        visible: root.collapsedGroups.includes("color") && root.collapsedGroups.length > 1
                    }
                    MenuEntry {
                        objectName: "overflowColor"
                        visible: root.collapsedGroups.includes("color")
                        text: "Text color…"
                        onTriggered: colorPicker.openPalette(overflowMenu)
                    }
                    MenuDivider {
                        theme: root.theme
                        visible: root.collapsedGroups.includes("table") && root.collapsedGroups.length > 1
                    }
                    MenuEntry {
                        objectName: "overflowInsertTable"
                        visible: root.collapsedGroups.includes("table")
                        text: "Insert table"; hint: "3×3"
                        onTriggered: controller.insertTable(3, 3)
                    }
                    MenuEntry {
                        objectName: "overflowInsertRowAbove"
                        visible: root.collapsedGroups.includes("table")
                        text: "Insert row above"
                        enabled: root.tableActive
                        onTriggered: controller.insertTableRowAbove()
                    }
                    MenuEntry {
                        objectName: "overflowInsertRow"
                        visible: root.collapsedGroups.includes("table")
                        text: "Insert row below"
                        enabled: root.tableActive
                        onTriggered: controller.insertTableRow()
                    }
                    MenuEntry {
                        objectName: "overflowDeleteRow"
                        visible: root.collapsedGroups.includes("table")
                        text: "Delete row"
                        enabled: root.tableActive
                        onTriggered: controller.deleteTableRow()
                    }
                    MenuEntry {
                        objectName: "overflowInsertColumnLeft"
                        visible: root.collapsedGroups.includes("table")
                        text: "Insert column left"
                        enabled: root.tableActive
                        onTriggered: controller.insertTableColumnLeft()
                    }
                    MenuEntry {
                        objectName: "overflowInsertColumn"
                        visible: root.collapsedGroups.includes("table")
                        text: "Insert column right"
                        enabled: root.tableActive
                        onTriggered: controller.insertTableColumn()
                    }
                    MenuEntry {
                        objectName: "overflowDeleteColumn"
                        visible: root.collapsedGroups.includes("table")
                        text: "Delete column"
                        enabled: root.tableActive
                        onTriggered: controller.deleteTableColumn()
                    }
                    MenuEntry {
                        objectName: "overflowDeleteTable"
                        visible: root.collapsedGroups.includes("table")
                        text: "Delete table"
                        enabled: root.tableActive
                        onTriggered: controller.deleteTable()
                    }
                }
            }
        }

        ToolButton {
            id: aiButton
            objectName: "aiButton"
            anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 6 }
            appRoot: root
            label: "✦ AI"; tip: "AI assistant"
            active: aiPanel.visible
            onClicked: aiPanel.visible = !aiPanel.visible
        }

        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1
            color: Qt.alpha(root.theme.muted, 0.5)
        }
    }

    PageSetup {
        id: pageSetup
        objectName: "pageSetup"
        appRoot: root
        controller: controller
        onClosed: editor.forceActiveFocus()
    }

    CommandPalette {
        id: commandPalette
        objectName: "commandPalette"
        appRoot: root
        commands: root.commands
        onClosed: editor.forceActiveFocus()
    }

    AiPanel {
        id: aiPanel
        objectName: "aiPanel"
        anchors { right: parent.right; top: toolbar.bottom; bottom: statusBar.top }
        width: Math.min(380, root.width * 0.45)
        visible: false
        appRoot: root
        ai: ai
        controller: controller
        onCloseRequested: {
            visible = false
            editor.forceActiveFocus()
        }
    }

    // The document as pages: sheets the size of the page, with the text
    // laid out on them by the document itself (it is paginated at the page
    // size, its margins the page margins). In a window narrower than a page,
    // everything scales down to fit.
    Flickable {
        id: flick
        anchors { left: parent.left; right: aiPanel.visible ? aiPanel.left : parent.right
                  top: toolbar.bottom; bottom: statusBar.top }
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
            visible: root.partEditing
            onPressed: root.finishPartEditing()
        }

        Item {
            id: pageColumn
            objectName: "pageColumn"
            readonly property size page: controller.pageSizePx
            // Between sheets, in the margins on either side of a page break.
            readonly property real gap: 32
            x: Math.max(flick.gutter, (flick.width - width * flick.zoom) / 2)
            y: flick.gutter
            width: page.width
            height: Math.max(controller.pageCount * page.height, editor.implicitHeight)
            scale: flick.zoom
            transformOrigin: Item.TopLeft

            Repeater {
                model: controller.pageCount
                Rectangle {
                    required property int index
                    objectName: "pageSheet"
                    y: index * pageColumn.page.height + (index > 0 ? pageColumn.gap / 2 : 0)
                    width: pageColumn.width
                    height: pageColumn.page.height - (index > 0 ? pageColumn.gap / 2 : 0)
                            - (index < controller.pageCount - 1 ? pageColumn.gap / 2 : 0)
                    color: root.pageColor
                    border.color: Qt.alpha(root.theme.foreground, 0.2)
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
                model: controller.pageCount
                Item {
                    id: pageParts
                    required property int index
                    readonly property var layout: controller.pageLayout
                    // The margin under the pointer, if any (see partHotspots).
                    readonly property string hovered: partHotspots.count > index ? partHotspots.itemAt(index).hovered : ""
                    y: index * pageColumn.page.height
                    width: pageColumn.width
                    height: pageColumn.page.height

                    PagePartView {
                        objectName: "headerView"
                        documentController: controller // not `controller:`, which would name this property itself
                        footer: false
                        page: pageParts.index + 1
                        x: root.mmToPx(pageParts.layout.left)
                        y: controller.headerYPx
                        width: pageColumn.width - x - root.mmToPx(pageParts.layout.right)
                        height: controller.headerHeightPx
                        color: Qt.alpha(root.theme.foreground, 0.75)
                        visible: controller.hasHeader && !(root.editingPart === "header" && root.editingPage === pageParts.index + 1)
                    }
                    PagePartView {
                        objectName: "footerView"
                        documentController: controller // not `controller:`, which would name this property itself
                        footer: true
                        page: pageParts.index + 1
                        x: root.mmToPx(pageParts.layout.left)
                        y: controller.footerYPx
                        width: pageColumn.width - x - root.mmToPx(pageParts.layout.right)
                        height: controller.footerHeightPx
                        color: Qt.alpha(root.theme.foreground, 0.75)
                        visible: controller.hasFooter && !(root.editingPart === "footer" && root.editingPage === pageParts.index + 1)
                    }
                    Text {
                        readonly property bool header: pageParts.hovered === "header"
                        visible: !root.partEditing && pageParts.hovered !== ""
                                 && (header ? !controller.hasHeader : !controller.hasFooter)
                        x: root.mmToPx(pageParts.layout.left)
                        // Centred in the margin, as the header or footer would be.
                        y: header ? (root.mmToPx(pageParts.layout.top) - height) / 2
                                  : pageParts.height - (root.mmToPx(pageParts.layout.bottom) + height) / 2
                        text: header ? "Double-click to add a header" : "Double-click to add a footer"
                        color: Qt.alpha(root.theme.foreground, 0.4)
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
                readOnly: controller.loading || controller.streamingEdit
                opacity: root.partEditing ? 0.45 : 1
                selectByMouse: true
                persistentSelection: true
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                color: root.theme.foreground
                selectionColor: root.theme.accent
                selectedTextColor: root.theme.background

                onTextChanged: if (root.autosaveActive && controller.dirty) autosaveTimer.restart()
                onCursorPositionChanged: root.syncSelection()
                onSelectionStartChanged: root.syncSelection()
                onSelectionEndChanged: root.syncSelection()
                onCursorRectangleChanged: flick.ensureVisible(cursorRectangle)

                // Undo/redo go through the controller rather than the TextEdit's
                // built-in handling, which knows nothing of pending formats or
                // the toolbar. Printable text is inserted by the controller too,
                // which groups typing into word-sized undo steps; see
                // DocumentController::typeText(). Return, Backspace, Delete and
                // input-method text stay with the TextEdit.
                Keys.onPressed: event => {
                    if (event.matches(StandardKey.Undo))
                        controller.undo()
                    else if (event.matches(StandardKey.Redo))
                        controller.redo()
                    else if (!(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                             && event.text.length > 0 && event.text >= " " && event.text !== "\x7f"
                             && controller.typeText(event.text))
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

            // Over each page's top and bottom margins: a double-click there
            // edits that page's header or footer; a click or drag places the
            // body's cursor, as one in the margin would anyway.
            Repeater {
                id: partHotspots
                model: controller.pageCount
                Item {
                    id: hotspot
                    required property int index
                    // "header" or "footer" under the pointer, else "".
                    readonly property string hovered: headerArea.containsMouse ? "header"
                                                    : footerArea.containsMouse ? "footer" : ""
                    y: index * pageColumn.page.height
                    width: pageColumn.width
                    height: pageColumn.page.height
                    visible: !root.partEditing

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
                        height: root.mmToPx(controller.pageLayout.top)
                        hoverEnabled: true
                        cursorShape: Qt.IBeamCursor
                        onPressed: mouse => hotspot.pressAt(headerArea, mouse)
                        onPositionChanged: mouse => { if (pressed) hotspot.dragAt(headerArea, mouse) }
                        // Once the gesture is over: the area hides when editing starts.
                        onDoubleClicked: Qt.callLater(root.editPart, "header", hotspot.index + 1)
                    }
                    MouseArea {
                        id: footerArea
                        objectName: "footerArea"
                        y: parent.height - height
                        width: parent.width
                        height: root.mmToPx(controller.pageLayout.bottom)
                        hoverEnabled: true
                        cursorShape: Qt.IBeamCursor
                        onPressed: mouse => hotspot.pressAt(footerArea, mouse)
                        onPositionChanged: mouse => { if (pressed) hotspot.dragAt(footerArea, mouse) }
                        // Once the gesture is over: the area hides when editing starts.
                        onDoubleClicked: Qt.callLater(root.editPart, "footer", hotspot.index + 1)
                    }
                }
            }

            // While a header or footer is edited: the body dims, and a click
            // anywhere outside it goes back to the body.
            MouseArea {
                objectName: "partEditingShield"
                anchors.fill: parent
                visible: root.partEditing
                cursorShape: Qt.IBeamCursor
                onPressed: mouse => {
                    const p = mapToItem(editor, mouse.x, mouse.y)
                    root.finishPartEditing(editor.positionAt(p.x, p.y))
                }
            }

            // The header and footer editors: the TextEdits their documents
            // are installed into (DocumentController::setPartDocument()),
            // shown over the page being edited.
            TextEdit {
                id: headerEditor
                objectName: "headerEditor"
                visible: root.editingPart === "header"
                y: (root.editingPage - 1) * pageColumn.page.height + controller.headerYPx
                x: root.mmToPx(controller.pageLayout.left)
                width: pageColumn.width - x - root.mmToPx(controller.pageLayout.right)
                height: implicitHeight
                padding: 0
                readOnly: controller.loading || controller.streamingEdit
                selectByMouse: true
                // Kept while the toolbar is used, which formats it.
                persistentSelection: true
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                color: root.theme.foreground
                selectionColor: root.theme.accent
                selectedTextColor: root.theme.background
                onTextChanged: if (root.autosaveActive && controller.dirty) autosaveTimer.restart()
                onCursorPositionChanged: if (visible) root.syncSelection()
                onSelectionStartChanged: if (visible) root.syncSelection()
                onSelectionEndChanged: if (visible) root.syncSelection()
                onCursorRectangleChanged: if (activeFocus) flick.ensureVisible(mapToItem(editor, cursorRectangle))
                Keys.onEscapePressed: root.finishPartEditing()
            }

            TextEdit {
                id: footerEditor
                objectName: "footerEditor"
                visible: root.editingPart === "footer"
                y: (root.editingPage - 1) * pageColumn.page.height + controller.footerYPx
                x: root.mmToPx(controller.pageLayout.left)
                width: pageColumn.width - x - root.mmToPx(controller.pageLayout.right)
                height: implicitHeight
                padding: 0
                readOnly: controller.loading || controller.streamingEdit
                selectByMouse: true
                // Kept while the toolbar is used, which formats it.
                persistentSelection: true
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                color: root.theme.foreground
                selectionColor: root.theme.accent
                selectedTextColor: root.theme.background
                onTextChanged: if (root.autosaveActive && controller.dirty) autosaveTimer.restart()
                onCursorPositionChanged: if (visible) root.syncSelection()
                onSelectionStartChanged: if (visible) root.syncSelection()
                onSelectionEndChanged: if (visible) root.syncSelection()
                onCursorRectangleChanged: if (activeFocus) flick.ensureVisible(mapToItem(editor, cursorRectangle))
                Keys.onEscapePressed: root.finishPartEditing()
            }

            // Where the header ends / the footer begins, and the edit bar:
            // fields, and the way back to the body. The toolbar formats the
            // header or footer meanwhile.
            Rectangle {
                visible: root.partEditing
                x: 0
                width: pageColumn.width
                height: 1
                y: root.editingPart === "header"
                   ? (root.editingPage - 1) * pageColumn.page.height
                     + Math.max(root.mmToPx(controller.pageLayout.top), headerEditor.y + headerEditor.height
                                                                        - (root.editingPage - 1) * pageColumn.page.height)
                   : root.editingPage * pageColumn.page.height
                     - Math.max(root.mmToPx(controller.pageLayout.bottom), root.editingPage * pageColumn.page.height - footerEditor.y)
                color: root.theme.accent
            }

            Rectangle {
                id: partBar
                objectName: "partBar"
                visible: root.partEditing
                readonly property real edge: root.editingPart === "header"
                    ? Math.max((root.editingPage - 1) * pageColumn.page.height + root.mmToPx(controller.pageLayout.top),
                               headerEditor.y + headerEditor.height)
                    : Math.min(root.editingPage * pageColumn.page.height - root.mmToPx(controller.pageLayout.bottom),
                               footerEditor.y)
                x: pageColumn.width - width - root.mmToPx(controller.pageLayout.right)
                y: root.editingPart === "header" ? edge + 6 : edge - height - 6
                width: partBarRow.implicitWidth + 8
                height: partBarRow.implicitHeight + 8
                radius: 5
                color: root.barColor
                border.color: root.theme.accent
                border.width: 1

                Row {
                    id: partBarRow
                    anchors.centerIn: parent
                    spacing: 2
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        rightPadding: 6
                        leftPadding: 4
                        text: root.editingPart === "header" ? "Editing header" : "Editing footer"
                        color: root.theme.accent
                        font.pixelSize: 12
                        font.bold: true
                    }
                    ToolButton {
                        objectName: "insertPageNumber"
                        appRoot: root
                        label: "# Page"
                        tip: "Insert the page number"
                        onClicked: root.insertPageField(false)
                    }
                    ToolButton {
                        objectName: "insertPageCount"
                        appRoot: root
                        label: "# Pages"
                        tip: "Insert the number of pages"
                        onClicked: root.insertPageField(true)
                    }
                    ToolButton {
                        objectName: "partDone"
                        appRoot: root
                        label: "Done"
                        tip: "Back to the document (Esc, or click the page)"
                        active: true
                        onClicked: root.finishPartEditing()
                    }
                }
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
                      right: pageIndicator.left; rightMargin: 10 }
            color: root.theme.foreground
            font.family: "monospace"
            font.pixelSize: 12
            elide: Text.ElideRight
            text: controller.busy ? root.busyText
                  : root.statusFlash !== "" ? root.statusFlash
                  : root.documentName + (controller.dirty ? " ●" : "")
        }

        // The page the cursor is on.
        Text {
            id: pageIndicator
            objectName: "pageIndicator"
            anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 10 }
            color: Qt.alpha(root.theme.foreground, 0.7)
            font.family: "monospace"
            font.pixelSize: 12
            text: root.partEditing ? "Editing " + root.editingPart + " · Esc or click the page to return"
                  : "Page " + root.currentPage() + " of " + controller.pageCount
        }

        Timer { id: flashTimer; interval: 2500; onTriggered: root.statusFlash = "" }
    }
}
