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

    // The toolbar groups now in its ⋯ menu (see Toolbar).
    readonly property alias collapsedGroups: toolbar.collapsedGroups

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
        controller.setEditTarget(part)
        editingPart = part
        editingPage = Math.max(1, Math.min(page, controller.pageCount))
        controller.setPartEditing(editingPage, theme.foreground)
        const partEditor = activePartEditor()
        partEditor.forceActiveFocus()
        partEditor.cursorPosition = partEditor.length
        syncSelection()
        pageView.ensureVisible(partEditor.mapToItem(pageView.editor, partEditor.cursorRectangle))
    }

    // Back to the body, its cursor where it was or, with `position`, there.
    // A header or footer grown past its margin gets the room it needs (see
    // DocumentController::fitPageParts()).
    function finishPartEditing(position) {
        if (!partEditing)
            return
        editingPart = ""
        controller.setEditTarget("")
        controller.fitPageParts()
        focusEditor()
        if (position !== undefined)
            pageView.editor.cursorPosition = position
        syncSelection()
    }

    // The TextEdit the selection and toolbar follow: the body's, or the
    // header's or footer's while one is edited.
    function activeEditor() {
        return partEditing ? activePartEditor() : pageView.editor
    }

    function activePartEditor() {
        return editingPart === "footer" ? pageView.footerEditor : pageView.headerEditor
    }

    // Edit bar: a page number or page count field at the cursor.
    function insertPageField(count) {
        const partEditor = activePartEditor()
        if (partEditor.selectedText !== "")
            partEditor.remove(partEditor.selectionStart, partEditor.selectionEnd)
        partEditor.cursorPosition = controller.insertPageField(editingPart, partEditor.cursorPosition, count)
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

    // Everything the command palette (Ctrl+K) offers (see Commands).
    readonly property var commands: paletteCommands.all

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
    // The cursor paragraph's heading level (0: body text).
    property int headingLevel: 0

    // The page the body's cursor is on, from 1.
    function currentPage() {
        return Math.min(controller.pageCount, Math.floor(pageView.editor.cursorRectangle.y / controller.pageSizePx.height) + 1)
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
        headingLevel = controller.currentHeadingLevel()
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

    // The body's cursor to `position` (the outline's sections), in view.
    function goToPosition(position) {
        finishPartEditing()
        pageView.editor.forceActiveFocus()
        pageView.editor.cursorPosition = position
    }

    // Insert citation (Ctrl+Shift+C).
    function openCitationPicker() {
        if (partEditing || controller.busy)
            return
        citationPicker.openPicker()
    }

    function insertBibliography() {
        if (partEditing)
            return
        if (!controller.insertBibliography())
            flash("The bibliography is up to date")
    }

    // A brief status-bar message.
    function flash(message) {
        statusFlash = message
        flashTimer.restart()
    }

    function showSidebar(tab) {
        sidebar.show(tab)
    }

    // Back to the body's text, after a menu or dialog closes.
    function focusEditor() {
        pageView.editor.forceActiveFocus()
    }

    // After a file dialog closes. Qt's own (non-native) file dialog is a
    // popup window, and Qt 6.8-6.10 leave keyboard focus with it once
    // hidden: typing went nowhere until the document was clicked.
    function refocusEditor() {
        root.requestActivate()
        focusEditor()
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
    minimumWidth: toolbar.minimumWidth
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
        controller.document = pageView.editor.textDocument
        controller.setPartDocument("header", pageView.headerEditor.textDocument)
        controller.setPartDocument("footer", pageView.footerEditor.textDocument)
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
        inkColor: root.theme.foreground
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
    Shortcut { sequences: ["Alt+F"]; onActivated: toolbar.openFileMenu() }
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
    // Paragraph styles: body text and headings (chapters, scenes).
    Shortcut { sequences: ["Ctrl+Alt+0"]; onActivated: controller.setHeadingLevel(0); enabled: !root.partEditing }
    Shortcut { sequences: ["Ctrl+Alt+1"]; onActivated: controller.setHeadingLevel(1); enabled: !root.partEditing }
    Shortcut { sequences: ["Ctrl+Alt+2"]; onActivated: controller.setHeadingLevel(2); enabled: !root.partEditing }
    Shortcut { sequences: ["Ctrl+Alt+3"]; onActivated: controller.setHeadingLevel(3); enabled: !root.partEditing }
    Shortcut { sequences: ["Ctrl+Shift+C"]; onActivated: root.openCitationPicker(); enabled: !root.partEditing }

    Toolbar {
        id: toolbar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        appRoot: root
        controller: controller
        aiPanel: aiPanel
        sidebar: sidebar
    }

    Sidebar {
        id: sidebar
        objectName: "sidebar"
        anchors { left: parent.left; top: toolbar.bottom; bottom: statusBar.top }
        width: Math.min(320, root.width * 0.35)
        visible: false
        appRoot: root
        controller: controller
        onCloseRequested: {
            visible = false
            root.focusEditor()
        }
    }

    CitationPicker {
        id: citationPicker
        objectName: "citationPicker"
        appRoot: root
        controller: controller
    }

    Commands {
        id: paletteCommands
        appRoot: root
        controller: controller
        toolbar: toolbar
        aiPanel: aiPanel
        sidebar: sidebar
    }


    PageSetup {
        id: pageSetup
        objectName: "pageSetup"
        appRoot: root
        controller: controller
        onClosed: root.focusEditor()
    }

    CommandPalette {
        id: commandPalette
        objectName: "commandPalette"
        appRoot: root
        commands: root.commands
        onClosed: root.focusEditor()
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
            root.focusEditor()
        }
    }

    PageView {
        id: pageView
        anchors { left: sidebar.visible ? sidebar.right : parent.left; right: aiPanel.visible ? aiPanel.left : parent.right
                  top: toolbar.bottom; bottom: statusBar.top }
        appRoot: root
        controller: controller
        onEdited: if (root.autosaveActive && controller.dirty) autosaveTimer.restart()
    }

    StatusBar {
        id: statusBar
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        appRoot: root
        controller: controller
    }

    Timer { id: flashTimer; interval: 2500; onTriggered: root.statusFlash = "" }
}
