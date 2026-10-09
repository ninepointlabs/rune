import QtQuick
import QtQuick.Window
import RuneNative

Window {
    id: root

    required property var theme

    // Stage 1 has no save dialog; Ctrl+S always writes here.
    readonly property string savePath: "/tmp/rune-native-test.odt"
    readonly property color barColor: theme.lighter_background !== undefined ? theme.lighter_background : theme.background
    readonly property string documentName: controller.currentPath !== ""
        ? controller.currentPath.substring(controller.currentPath.lastIndexOf("/") + 1) : "Untitled"
    // Brief status-bar message ("Saved"); empty shows the document name.
    property string statusFlash: ""

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

    function save() {
        statusFlash = controller.saveToOdf(savePath) ? "Saved " + savePath : "Save failed: " + savePath
        flashTimer.restart()
    }

    width: 1000
    height: 800
    visible: true
    color: theme.background
    title: documentName + (controller.dirty ? " ●" : "") + " — Rune"

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

    Shortcut { sequence: StandardKey.Save; onActivated: root.save() }
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
            text: root.statusFlash !== "" ? root.statusFlash
                  : root.documentName + (controller.dirty ? " ●" : "")
        }

        Timer { id: flashTimer; interval: 2500; onTriggered: root.statusFlash = "" }
    }
}
