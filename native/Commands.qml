import QtQuick
import Rune

// Everything the command palette (Ctrl+K) offers, in the order it lists them
// when nothing is typed: one list per feature, joined in `all`. A command is
// {group, name, hint (optional), available, run}: `available` says whether
// it applies right now; hints match the window's shortcuts. A new feature
// adds its list here.
QtObject {
    id: commands

    required property var appRoot   // the Main.qml Window instance
    required property DocumentController controller
    required property Item toolbar
    required property Item aiPanel
    required property Item sidebar

    readonly property var all: [].concat(file, page, edit, format, writing, lists, tables, citations, ai)

    readonly property var always: () => true
    // Lists, tables and the AI are the body's only, not a header's or footer's.
    readonly property var body: () => !appRoot.partEditing
    readonly property var inTable: () => !appRoot.partEditing && controller.isInTable()

    readonly property var file: {
        const idle = () => !controller.busy
        return [
            {group: "File", name: "New", hint: "Ctrl+N", available: idle, run: () => appRoot.requestNew()},
            {group: "File", name: "Open…", hint: "Ctrl+O", available: idle, run: () => appRoot.requestOpen()},
            {group: "File", name: "Save", hint: "Ctrl+S", available: idle, run: () => appRoot.save()},
            {group: "File", name: "Save As…", hint: "Ctrl+Shift+S", available: idle, run: () => appRoot.saveAs()},
            {group: "File", name: "Page setup…", available: always, run: () => appRoot.openPageSetup()},
            {group: "File", name: appRoot.autosaveEnabled ? "Turn autosave off" : "Turn autosave on", available: always,
             run: () => appRoot.autosaveEnabled = !appRoot.autosaveEnabled},
            {group: "Help", name: "About Rune", available: always, run: () => appRoot.openAbout()}
        ]
    }

    readonly property var page: [
        {group: "Page", name: "Edit header", available: always, run: () => appRoot.editPart("header", appRoot.currentPage())},
        {group: "Page", name: "Edit footer", available: always, run: () => appRoot.editPart("footer", appRoot.currentPage())},
        {group: "Page", name: "Page numbers", available: always, run: () => appRoot.addPageNumbers()}
    ]

    readonly property var edit: [
        {group: "Edit", name: "Undo", hint: "Ctrl+Z", available: () => controller.canUndo && !controller.loading,
         run: () => controller.undo()},
        {group: "Edit", name: "Redo", hint: "Ctrl+Shift+Z", available: () => controller.canRedo && !controller.loading,
         run: () => controller.redo()}
    ]

    readonly property var format: {
        const align = (name, value) => ({group: "Format", name: name, available: always,
                                         run: () => controller.setAlignment(value)})
        return [
            {group: "Format", name: "Bold", hint: "Ctrl+B", available: always, run: () => controller.toggleBold()},
            {group: "Format", name: "Italic", hint: "Ctrl+I", available: always, run: () => controller.toggleItalic()},
            {group: "Format", name: "Underline", hint: "Ctrl+U", available: always, run: () => controller.toggleUnderline()},
            {group: "Format", name: "Font…", available: always, run: () => toolbar.openFontList()},
            {group: "Format", name: "Increase font size", available: always, run: () => appRoot.stepFontSize(1)},
            {group: "Format", name: "Decrease font size", available: always, run: () => appRoot.stepFontSize(-1)},
            {group: "Format", name: "Text color…", available: always, run: () => toolbar.openColorPalette()},
            {group: "Format", name: "Remove text color", available: () => appRoot.textColor !== "auto",
             run: () => controller.setTextColor("auto")},
            align("Align left", Qt.AlignLeft),
            align("Align center", Qt.AlignHCenter),
            align("Align right", Qt.AlignRight)
        ]
    }

    // Author tooling: headings, the outline, story notes.
    readonly property var writing: {
        const heading = (name, level, hint) => ({group: "Write", name: name, hint: hint, available: body,
                                                 run: () => controller.setHeadingLevel(level)})
        return [
            heading("Heading 1 (chapter)", 1, "Ctrl+Alt+1"),
            heading("Heading 2 (scene)", 2, "Ctrl+Alt+2"),
            heading("Heading 3", 3, "Ctrl+Alt+3"),
            heading("Body text", 0, "Ctrl+Alt+0"),
            {group: "Write", name: "Show outline", available: always, run: () => sidebar.show("outline")},
            {group: "Write", name: "Show story notes", available: always, run: () => sidebar.show("notes")},
            {group: "Write", name: "Add character…", available: always, run: () => sidebar.addNote("character")},
            {group: "Write", name: "Add place…", available: always, run: () => sidebar.addNote("place")},
            {group: "Write", name: "Add research note…", available: always, run: () => sidebar.addNote("research")},
            {group: "Write", name: sidebar.visible ? "Hide sidebar" : "Show sidebar", available: always,
             run: () => sidebar.visible = !sidebar.visible}
        ]
    }

    readonly property var citations: {
        const style = (id, name) => ({group: "Cite", name: "Citation style: " + name,
                                      hint: controller.citationStyle === id ? "✓" : "", available: always,
                                      run: () => controller.citationStyle = id})
        return [
            {group: "Cite", name: "Insert citation…", hint: "Ctrl+Shift+C",
             available: () => body() && controller.sources.length > 0, run: () => appRoot.openCitationPicker()},
            {group: "Cite", name: "Insert bibliography", available: body, run: () => appRoot.insertBibliography()},
            {group: "Cite", name: "Show sources", available: always, run: () => sidebar.show("sources")},
            style("apa", "APA 7"),
            style("mla", "MLA 9"),
            style("chicago", "Chicago (author-date)"),
            style("ieee", "IEEE")
        ]
    }

    readonly property var lists: [
        {group: "List", name: "Bullet list", available: body, run: () => controller.toggleBulletList()},
        {group: "List", name: "Numbered list", available: body, run: () => controller.toggleNumberedList()}
    ]

    readonly property var tables: [
        {group: "Table", name: "Insert table", hint: "3×3", available: body, run: () => controller.insertTable(3, 3)},
        {group: "Table", name: "Insert row above", available: inTable, run: () => controller.insertTableRowAbove()},
        {group: "Table", name: "Insert row below", available: inTable, run: () => controller.insertTableRow()},
        {group: "Table", name: "Delete row", available: inTable, run: () => controller.deleteTableRow()},
        {group: "Table", name: "Insert column left", available: inTable, run: () => controller.insertTableColumnLeft()},
        {group: "Table", name: "Insert column right", available: inTable, run: () => controller.insertTableColumn()},
        {group: "Table", name: "Delete column", available: inTable, run: () => controller.deleteTableColumn()},
        {group: "Table", name: "Delete table", available: inTable, run: () => controller.deleteTable()}
    ]

    readonly property var ai: [
        {group: "AI", name: "Edit with AI", hint: "Ctrl+Shift+E", available: body, run: () => aiPanel.startEdit()},
        {group: "AI", name: aiPanel.visible ? "Hide AI panel" : "Show AI panel", available: always,
         run: () => aiPanel.visible = !aiPanel.visible}
    ]
}
