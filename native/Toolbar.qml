import QtQuick
import Rune

// The toolbar, in groups. When the window is too narrow for all of them,
// the lowest-priority groups (collapseOrder) move into the ⋯ menu instead of
// running under the AI button; File, Save, B/I/U and the font controls
// always stay. Its state (bold, alignment, …) is the window's, refreshed
// from the controller on formatChanged.
Rectangle {
    id: bar

    required property var appRoot   // the Main.qml Window instance
    required property DocumentController controller
    required property Item aiPanel
    required property Item sidebar
    readonly property var theme: appRoot.theme

    // Toolbar groups that move into the ⋯ menu when the window is too
    // narrow, first to go first. The rest always stay on the toolbar.
    readonly property var collapseOrder: ["table", "color", "align", "lists", "heading", "history"]
    // The groups in the ⋯ menu now: as many from collapseOrder as it takes
    // for the toolbar (with the ⋯ button) to fit left of the AI button.
    readonly property var collapsedGroups: {
        const groups = {history: historyGroup, heading: headingGroup, lists: listsGroup, align: alignGroup, color: colorGroup,
                        table: tableGroup}
        const gap = toolRow.spacing
        const room = width - toolRow.anchors.leftMargin - aiButton.width - aiButton.anchors.rightMargin - gap
                     - sidebarButton.width - sidebarButton.anchors.rightMargin
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
    // File, Save, B/I/U, the font controls, ⋯ and AI: what never collapses.
    readonly property real minimumWidth: fileGroup.width + styleGroup.width + fontGroup.width + overflowWidth
                                         + aiButton.width + sidebarButton.width + 36

    // For the command palette and shortcuts.
    function openFileMenu() {
        fileMenu.openMenu()
    }
    function openFontList() {
        fontPicker.openList()
    }
    // From its toolbar button, or the ⋯ menu while that holds it.
    function openColorPalette() {
        colorPicker.openPalette(colorGroup.visible ? colorPicker : overflowMenu)
    }

    height: 40
    z: 2 // tooltips overlap the document view
    color: bar.appRoot.barColor

    component ToolSeparator: Rectangle {
        width: 1
        height: 20
        anchors.verticalCenter: parent.verticalCenter
        color: bar.theme.muted
    }

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
                appRoot: bar.appRoot
                available: !bar.controller.busy
            }
            // Shows where the document stands: "✓ Saved" (dimmed) once
            // autosave has written it, "Saving…", or "Save failed" (click
            // to retry); a plain "Save" when it has unsaved changes or
            // autosave doesn't apply.
            ToolButton {
                objectName: "saveButton"
                appRoot: bar.appRoot
                width: 88 // the widest label, so the toolbar doesn't shift
                label: bar.appRoot.saving && bar.controller.busy ? "Saving…"
                     : bar.appRoot.saveFailed ? "Save failed"
                     : bar.appRoot.autosaveActive && !bar.controller.dirty ? "✓ Saved"
                     : "Save"
                labelColor: bar.appRoot.saveFailed ? bar.theme.red
                          : label === "Save" ? bar.theme.foreground
                          : Qt.alpha(bar.theme.foreground, 0.55)
                tip: bar.appRoot.saveFailed ? bar.appRoot.saveError + " · click to retry"
                     : bar.appRoot.autosaveActive ? "Autosave is on · Save now (Ctrl+S)"
                     : "Save (Ctrl+S)"
                available: !bar.controller.busy
                onClicked: bar.appRoot.save()
            }
        }

        Row {
            id: historyGroup
            objectName: "historyGroup"
            spacing: 4
            visible: !bar.collapsedGroups.includes("history")
            ToolSeparator {}
            ToolButton {
                objectName: "undoButton"
                appRoot: bar.appRoot
                label: "↶"; tip: "Undo (Ctrl+Z)"
                available: bar.controller.canUndo && !bar.controller.loading
                onClicked: bar.controller.undo()
            }
            ToolButton {
                objectName: "redoButton"
                appRoot: bar.appRoot
                label: "↷"; tip: "Redo (Ctrl+Shift+Z)"
                available: bar.controller.canRedo && !bar.controller.loading
                onClicked: bar.controller.redo()
            }
        }

        Row {
            id: styleGroup
            spacing: 4
            ToolSeparator {}
            ToolButton {
                objectName: "boldButton"
                appRoot: bar.appRoot
                label: "B"; bold: true; tip: "Bold (Ctrl+B)"
                active: bar.appRoot.boldActive
                onClicked: bar.controller.toggleBold()
            }
            ToolButton {
                appRoot: bar.appRoot
                label: "I"; italic: true; tip: "Italic (Ctrl+I)"
                active: bar.appRoot.italicActive
                onClicked: bar.controller.toggleItalic()
            }
            ToolButton {
                appRoot: bar.appRoot
                label: "U"; underline: true; tip: "Underline (Ctrl+U)"
                active: bar.appRoot.underlineActive
                onClicked: bar.controller.toggleUnderline()
            }
        }

        // The paragraph's style: body text or a heading (chapters, scenes).
        Row {
            id: headingGroup
            enabled: !bar.appRoot.partEditing
            opacity: enabled ? 1 : 0.4
            objectName: "headingGroup"
            spacing: 4
            visible: !bar.collapsedGroups.includes("heading")
            ToolSeparator {}
            ToolMenu {
                objectName: "headingButton"
                menuName: "headingMenu"
                appRoot: bar.appRoot
                // Compact: "Body", "H1", ...; the menu names them in full.
                label: bar.appRoot.headingLevel > 0 ? "H" + bar.appRoot.headingLevel : "Body"
                onClosed: bar.appRoot.focusEditor()
                MenuEntry {
                    objectName: "headingItem0"
                    text: "Body text"; hint: bar.appRoot.headingLevel === 0 ? "✓" : "Ctrl+Alt+0"
                    onTriggered: bar.controller.setHeadingLevel(0)
                }
                MenuEntry {
                    objectName: "headingItem1"
                    text: "Heading 1"; hint: bar.appRoot.headingLevel === 1 ? "✓" : "Ctrl+Alt+1"
                    onTriggered: bar.controller.setHeadingLevel(1)
                }
                MenuEntry {
                    objectName: "headingItem2"
                    text: "Heading 2"; hint: bar.appRoot.headingLevel === 2 ? "✓" : "Ctrl+Alt+2"
                    onTriggered: bar.controller.setHeadingLevel(2)
                }
                MenuEntry {
                    objectName: "headingItem3"
                    text: "Heading 3"; hint: bar.appRoot.headingLevel === 3 ? "✓" : "Ctrl+Alt+3"
                    onTriggered: bar.controller.setHeadingLevel(3)
                }
            }
        }

        Row {
            id: listsGroup
            enabled: !bar.appRoot.partEditing
            opacity: enabled ? 1 : 0.4
            objectName: "listsGroup"
            spacing: 4
            visible: !bar.collapsedGroups.includes("lists")
            ToolSeparator {}
            ToolButton {
                objectName: "bulletButton"
                appRoot: bar.appRoot
                label: "•"; tip: "Bullet list (Tab / Shift+Tab to nest)"
                active: bar.appRoot.bulletActive
                onClicked: bar.controller.toggleBulletList()
            }
            ToolButton {
                objectName: "numberedButton"
                appRoot: bar.appRoot
                label: "1."; tip: "Numbered list (Tab / Shift+Tab to nest)"
                active: bar.appRoot.numberedActive
                onClicked: bar.controller.toggleNumberedList()
            }
        }

        Row {
            id: alignGroup
            objectName: "alignGroup"
            spacing: 4
            visible: !bar.collapsedGroups.includes("align")
            ToolSeparator {}
            // Shows the alignment at the cursor; the menu sets it.
            ToolMenu {
                objectName: "alignButton"
                menuName: "alignMenu"
                appRoot: bar.appRoot
                icon: AlignIcon { alignment: bar.appRoot.alignment; color: bar.theme.foreground }
                onClosed: bar.appRoot.focusEditor()
                MenuEntry {
                    objectName: "alignLeftItem"
                    text: "Align left"; hint: bar.appRoot.alignment === Qt.AlignLeft ? "✓" : ""
                    onTriggered: bar.controller.setAlignment(Qt.AlignLeft)
                }
                MenuEntry {
                    objectName: "alignCenterItem"
                    text: "Align center"; hint: bar.appRoot.alignment === Qt.AlignHCenter ? "✓" : ""
                    onTriggered: bar.controller.setAlignment(Qt.AlignHCenter)
                }
                MenuEntry {
                    objectName: "alignRightItem"
                    text: "Align right"; hint: bar.appRoot.alignment === Qt.AlignRight ? "✓" : ""
                    onTriggered: bar.controller.setAlignment(Qt.AlignRight)
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
                appRoot: bar.appRoot
                controller: bar.controller
                currentFamily: bar.appRoot.fontFamily
                onClosed: bar.appRoot.focusEditor()
            }
            ToolButton {
                objectName: "fontSmallerButton"
                appRoot: bar.appRoot
                label: "−"; tip: "Decrease font size"
                onClicked: bar.appRoot.stepFontSize(-1)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: 40
                horizontalAlignment: Text.AlignHCenter
                text: bar.appRoot.fontSize + "pt"
                color: bar.theme.foreground
                font.pixelSize: 13
            }
            ToolButton {
                objectName: "fontLargerButton"
                appRoot: bar.appRoot
                label: "+"; tip: "Increase font size"
                onClicked: bar.appRoot.stepFontSize(1)
            }
        }

        Row {
            id: colorGroup
            objectName: "colorGroup"
            spacing: 4
            visible: !bar.collapsedGroups.includes("color")
            ToolSeparator {}
            ColorPicker {
                id: colorPicker
                objectName: "colorButton"
                anchors.verticalCenter: parent.verticalCenter
                appRoot: bar.appRoot
                controller: bar.controller
                currentColor: bar.appRoot.textColor
                onClosed: bar.appRoot.focusEditor()
            }
        }

        Row {
            id: tableGroup
            enabled: !bar.appRoot.partEditing
            opacity: enabled ? 1 : 0.4
            objectName: "tableGroup"
            spacing: 4
            visible: !bar.collapsedGroups.includes("table")
            ToolSeparator {}
            ToolMenu {
                objectName: "tableButton"
                menuName: "tableMenu"
                appRoot: bar.appRoot
                label: "⊞ Table"
                onClosed: bar.appRoot.focusEditor()
                MenuEntry {
                    objectName: "insertTableItem"
                    text: "Insert table"; hint: "3×3"
                    onTriggered: bar.controller.insertTable(3, 3)
                }
                MenuDivider { theme: bar.theme }
                MenuEntry {
                    objectName: "insertRowAboveItem"
                    text: "Insert row above"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.insertTableRowAbove()
                }
                MenuEntry {
                    objectName: "insertRowItem"
                    text: "Insert row below"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.insertTableRow()
                }
                MenuEntry {
                    objectName: "deleteRowItem"
                    text: "Delete row"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.deleteTableRow()
                }
                MenuDivider { theme: bar.theme }
                MenuEntry {
                    objectName: "insertColumnLeftItem"
                    text: "Insert column left"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.insertTableColumnLeft()
                }
                MenuEntry {
                    objectName: "insertColumnItem"
                    text: "Insert column right"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.insertTableColumn()
                }
                MenuEntry {
                    objectName: "deleteColumnItem"
                    text: "Delete column"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.deleteTableColumn()
                }
                MenuDivider { theme: bar.theme }
                MenuEntry {
                    objectName: "deleteTableItem"
                    text: "Delete table"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.deleteTable()
                }
            }
        }

        // The groups that didn't fit, as menu entries, in toolbar order.
        Row {
            spacing: 4
            visible: bar.collapsedGroups.length > 0
            ToolSeparator {}
            ToolMenu {
                id: overflowMenu
                objectName: "overflowButton"
                menuName: "overflowMenu"
                appRoot: bar.appRoot
                label: "⋯"
                caret: false
                onClosed: if (!colorPicker.paletteOpen) bar.appRoot.focusEditor()

                MenuEntry {
                    objectName: "overflowUndo"
                    visible: bar.collapsedGroups.includes("history")
                    text: "Undo"; hint: "Ctrl+Z"
                    enabled: bar.controller.canUndo && !bar.controller.loading
                    onTriggered: bar.controller.undo()
                }
                MenuEntry {
                    objectName: "overflowRedo"
                    visible: bar.collapsedGroups.includes("history")
                    text: "Redo"; hint: "Ctrl+Shift+Z"
                    enabled: bar.controller.canRedo && !bar.controller.loading
                    onTriggered: bar.controller.redo()
                }
                MenuDivider {
                    theme: bar.theme
                    visible: bar.collapsedGroups.includes("heading") && bar.collapsedGroups.includes("history")
                }
                MenuEntry {
                    objectName: "overflowHeading0"
                    visible: bar.collapsedGroups.includes("heading")
                    enabled: !bar.appRoot.partEditing
                    text: "Body text"; hint: bar.appRoot.headingLevel === 0 ? "✓" : "Ctrl+Alt+0"
                    onTriggered: bar.controller.setHeadingLevel(0)
                }
                MenuEntry {
                    objectName: "overflowHeading1"
                    visible: bar.collapsedGroups.includes("heading")
                    enabled: !bar.appRoot.partEditing
                    text: "Heading 1"; hint: bar.appRoot.headingLevel === 1 ? "✓" : "Ctrl+Alt+1"
                    onTriggered: bar.controller.setHeadingLevel(1)
                }
                MenuEntry {
                    objectName: "overflowHeading2"
                    visible: bar.collapsedGroups.includes("heading")
                    enabled: !bar.appRoot.partEditing
                    text: "Heading 2"; hint: bar.appRoot.headingLevel === 2 ? "✓" : "Ctrl+Alt+2"
                    onTriggered: bar.controller.setHeadingLevel(2)
                }
                MenuEntry {
                    objectName: "overflowHeading3"
                    visible: bar.collapsedGroups.includes("heading")
                    enabled: !bar.appRoot.partEditing
                    text: "Heading 3"; hint: bar.appRoot.headingLevel === 3 ? "✓" : "Ctrl+Alt+3"
                    onTriggered: bar.controller.setHeadingLevel(3)
                }
                MenuDivider {
                    theme: bar.theme
                    visible: bar.collapsedGroups.includes("lists")
                             && (bar.collapsedGroups.includes("history") || bar.collapsedGroups.includes("heading"))
                }
                MenuEntry {
                    objectName: "overflowBullet"
                    visible: bar.collapsedGroups.includes("lists")
                    text: "Bullet list"; hint: bar.appRoot.bulletActive ? "✓" : ""
                    onTriggered: bar.controller.toggleBulletList()
                }
                MenuEntry {
                    objectName: "overflowNumbered"
                    visible: bar.collapsedGroups.includes("lists")
                    text: "Numbered list"; hint: bar.appRoot.numberedActive ? "✓" : ""
                    onTriggered: bar.controller.toggleNumberedList()
                }
                MenuDivider {
                    theme: bar.theme
                    visible: bar.collapsedGroups.includes("align")
                             && (bar.collapsedGroups.includes("history") || bar.collapsedGroups.includes("lists"))
                }
                MenuEntry {
                    objectName: "overflowAlignLeft"
                    visible: bar.collapsedGroups.includes("align")
                    text: "Align left"; hint: bar.appRoot.alignment === Qt.AlignLeft ? "✓" : ""
                    onTriggered: bar.controller.setAlignment(Qt.AlignLeft)
                }
                MenuEntry {
                    objectName: "overflowAlignCenter"
                    visible: bar.collapsedGroups.includes("align")
                    text: "Align center"; hint: bar.appRoot.alignment === Qt.AlignHCenter ? "✓" : ""
                    onTriggered: bar.controller.setAlignment(Qt.AlignHCenter)
                }
                MenuEntry {
                    objectName: "overflowAlignRight"
                    visible: bar.collapsedGroups.includes("align")
                    text: "Align right"; hint: bar.appRoot.alignment === Qt.AlignRight ? "✓" : ""
                    onTriggered: bar.controller.setAlignment(Qt.AlignRight)
                }
                MenuDivider {
                    theme: bar.theme
                    visible: bar.collapsedGroups.includes("color") && bar.collapsedGroups.length > 1
                }
                MenuEntry {
                    objectName: "overflowColor"
                    visible: bar.collapsedGroups.includes("color")
                    text: "Text color…"
                    onTriggered: colorPicker.openPalette(overflowMenu)
                }
                MenuDivider {
                    theme: bar.theme
                    visible: bar.collapsedGroups.includes("table") && bar.collapsedGroups.length > 1
                }
                MenuEntry {
                    objectName: "overflowInsertTable"
                    visible: bar.collapsedGroups.includes("table")
                    text: "Insert table"; hint: "3×3"
                    onTriggered: bar.controller.insertTable(3, 3)
                }
                MenuEntry {
                    objectName: "overflowInsertRowAbove"
                    visible: bar.collapsedGroups.includes("table")
                    text: "Insert row above"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.insertTableRowAbove()
                }
                MenuEntry {
                    objectName: "overflowInsertRow"
                    visible: bar.collapsedGroups.includes("table")
                    text: "Insert row below"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.insertTableRow()
                }
                MenuEntry {
                    objectName: "overflowDeleteRow"
                    visible: bar.collapsedGroups.includes("table")
                    text: "Delete row"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.deleteTableRow()
                }
                MenuEntry {
                    objectName: "overflowInsertColumnLeft"
                    visible: bar.collapsedGroups.includes("table")
                    text: "Insert column left"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.insertTableColumnLeft()
                }
                MenuEntry {
                    objectName: "overflowInsertColumn"
                    visible: bar.collapsedGroups.includes("table")
                    text: "Insert column right"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.insertTableColumn()
                }
                MenuEntry {
                    objectName: "overflowDeleteColumn"
                    visible: bar.collapsedGroups.includes("table")
                    text: "Delete column"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.deleteTableColumn()
                }
                MenuEntry {
                    objectName: "overflowDeleteTable"
                    visible: bar.collapsedGroups.includes("table")
                    text: "Delete table"
                    enabled: bar.appRoot.tableActive
                    onTriggered: bar.controller.deleteTable()
                }
            }
        }
    }

    // The author's sidebar: outline, notes, sources.
    ToolButton {
        id: sidebarButton
        objectName: "sidebarButton"
        anchors { verticalCenter: parent.verticalCenter; right: aiButton.left; rightMargin: 4 }
        appRoot: bar.appRoot
        label: "☰"; tip: "Outline, notes and sources"
        active: bar.sidebar.visible
        onClicked: bar.sidebar.visible = !bar.sidebar.visible
    }

    ToolButton {
        id: aiButton
        objectName: "aiButton"
        anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 6 }
        appRoot: bar.appRoot
        label: "✦ AI"; tip: "AI assistant"
        active: bar.aiPanel.visible
        onClicked: bar.aiPanel.visible = !bar.aiPanel.visible
    }

    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 1
        color: Qt.alpha(bar.theme.muted, 0.5)
    }
}
