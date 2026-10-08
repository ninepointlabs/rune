import QtQuick

// Second toolbar row whose tools follow the cursor: text tools while
// something is selected, table tools inside a table, otherwise insert and
// paragraph-spacing tools. Always 40px tall so the document never jumps.
Rectangle {
    id: barRoot
    required property var appRoot   // the Main.qml Window instance

    readonly property string mode: appRoot.selectionRects.length > 0 ? "selection"
                                 : appRoot.inTable ? "table" : "default"

    height: 40
    z: 1 // tooltips overlap the document view; the Toolbar's style popup overlaps us
    color: appRoot.theme.lighter_background !== undefined ? appRoot.theme.lighter_background : appRoot.theme.background

    // Selection: font, size, color, format painter.
    Row {
        id: selectionTools
        readonly property bool shown: barRoot.mode === "selection"
        anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 4 }
        spacing: 4
        opacity: shown ? 1 : 0
        visible: opacity > 0
        enabled: shown
        Behavior on opacity { NumberAnimation { duration: 120 } }

        ToolButton {
            appRoot: barRoot.appRoot
            fixedWidth: 130
            label: (appRoot.formattingState.CharFontName || "Font") + " ▾"
            tip: "Font: click for next (" + appRoot.fontNames.join(", ") + ")"
            border.color: Qt.alpha(appRoot.theme.foreground, 0.2)
            border.width: 1
            onClicked: appRoot.cycleFont()
        }

        ToolButton {
            appRoot: barRoot.appRoot
            label: "−"; tip: "Decrease font size"
            onClicked: appRoot.stepFontSize(-1)
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: 40
            horizontalAlignment: Text.AlignHCenter
            text: isNaN(barRoot.appRoot.currentFontSize) ? "—" : barRoot.appRoot.currentFontSize + "pt"
            color: barRoot.appRoot.theme.foreground
            opacity: barRoot.appRoot.docId >= 0 ? 1 : 0.5
            font.pixelSize: 13
        }
        ToolButton {
            appRoot: barRoot.appRoot
            label: "+"; tip: "Increase font size"
            onClicked: appRoot.stepFontSize(1)
        }

        ToolSeparator { appRoot: barRoot.appRoot }

        ToolButton {
            id: colorButton
            appRoot: barRoot.appRoot
            readonly property var current: appRoot.currentColorIndex >= 0
                ? appRoot.textColors[appRoot.currentColorIndex] : ["Custom", ""]
            label: "A"; bold: true
            tip: "Text color: " + current[0] + " (click for next)"
            onClicked: appRoot.cycleColor()

            // Swatch of the color at the cursor; automatic shows as the foreground.
            Rectangle {
                anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 4 }
                width: 16; height: 3; radius: 1
                color: colorButton.appRoot.currentColorHex === "auto" ? colorButton.appRoot.theme.foreground
                                                                      : colorButton.appRoot.currentColorHex
            }
        }

        ToolSeparator { appRoot: barRoot.appRoot }

        ToolButton {
            appRoot: barRoot.appRoot
            label: "🖌"
            tip: "Format painter: click to copy formatting once, double-click to keep painting (Esc stops)"
            active: appRoot.paintMode
            // A double-click arrives as clicked then doubleClicked; the
            // first click captures, the second makes the mode sticky.
            onClicked: appRoot.paintMode ? appRoot.exitPaintMode() : appRoot.startPaintMode(false)
            onDoubleClicked: appRoot.startPaintMode(true)
        }
    }

    // Cursor in a table: AutoFit and +Row.
    Row {
        id: tableTools
        readonly property bool shown: barRoot.mode === "table"
        anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 4 }
        spacing: 4
        opacity: shown ? 1 : 0
        visible: opacity > 0
        enabled: shown
        Behavior on opacity { NumberAnimation { duration: 120 } }

        ToolButton {
            appRoot: barRoot.appRoot
            label: "⊞ AutoFit"; tip: "Fit column widths to their contents"
            available: appRoot.inTable
            onClicked: appRoot.applyTable("autofit")
        }
        ToolButton {
            appRoot: barRoot.appRoot
            label: "+Row"; tip: "Insert a row below"
            available: appRoot.inTable
            onClicked: appRoot.applyTable("insert_row")
        }
        ToolButton {
            appRoot: barRoot.appRoot
            label: "−Row"; tip: "Delete this row"
            available: appRoot.inTable
            onClicked: appRoot.applyTable("delete_row")
        }
        ToolButton {
            appRoot: barRoot.appRoot
            label: "+Col"; tip: "Insert a column after this one"
            available: appRoot.inTable
            onClicked: appRoot.applyTable("insert_column")
        }
        ToolButton {
            appRoot: barRoot.appRoot
            label: "−Col"; tip: "Delete this column"
            available: appRoot.inTable
            onClicked: appRoot.applyTable("delete_column")
        }
    }

    // Default: insert a table, paragraph spacing.
    Row {
        id: defaultTools
        readonly property bool shown: barRoot.mode === "default"
        anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 4 }
        spacing: 4
        opacity: shown ? 1 : 0
        visible: opacity > 0
        enabled: shown
        Behavior on opacity { NumberAnimation { duration: 120 } }

        // Insert is a fixed 3x3 for now (a size picker could come later).
        ToolButton {
            appRoot: barRoot.appRoot
            label: "⊞ Table"; tip: "Insert a 3×3 table"
            onClicked: appRoot.applyTable("insert", 3, 3)
        }

        ToolSeparator { appRoot: barRoot.appRoot }

        // Paragraph spacing: directional only. LOK does not apply exact
        // point values or keep-together/widow-orphan headlessly (see
        // engine/README.md and the `para` command's comment) — these
        // buttons use the LO-native spacing step instead of a fixed pt.
        ToolButton {
            appRoot: barRoot.appRoot
            label: "¶−"; tip: "Decrease paragraph spacing"
            onClicked: appRoot.applyParaSpacing("decrease")
        }
        ToolButton {
            appRoot: barRoot.appRoot
            label: "¶+"; tip: "Increase paragraph spacing"
            onClicked: appRoot.applyParaSpacing("increase")
        }
    }

    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 1
        color: barRoot.appRoot.theme.muted
    }
}
