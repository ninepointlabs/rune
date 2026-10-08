import QtQuick

Rectangle {
    id: toolbarRoot
    required property var appRoot   // the Main.qml Window instance

    height: 40
    z: 2 // tooltips overlap the document view
    color: appRoot.theme.lighter_background !== undefined ? appRoot.theme.lighter_background : appRoot.theme.background

    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 1
        color: Qt.alpha(toolbarRoot.appRoot.theme.muted, 0.5)
    }

    Row {
        anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 4 }
        spacing: 4

        ToolButton {
            appRoot: toolbarRoot.appRoot
            label: "B"; bold: true; tip: "Bold (Ctrl+B)"
            active: appRoot.formattingState.Bold === true
            onClicked: appRoot.applyFormat(".uno:Bold")
        }
        ToolButton {
            appRoot: toolbarRoot.appRoot
            label: "I"; italic: true; tip: "Italic (Ctrl+I)"
            active: appRoot.formattingState.Italic === true
            onClicked: appRoot.applyFormat(".uno:Italic")
        }
        ToolButton {
            appRoot: toolbarRoot.appRoot
            label: "U"; underline: true; tip: "Underline (Ctrl+U)"
            active: appRoot.formattingState.Underline === true
            onClicked: appRoot.applyFormat(".uno:Underline")
        }

        ToolSeparator { appRoot: toolbarRoot.appRoot }

        Repeater {
            model: [["Normal", "Normal", "Normal paragraph"], ["H1", "Heading 1", "Heading 1"],
                    ["H2", "Heading 2", "Heading 2"], ["H3", "Heading 3", "Heading 3"]]
            ToolButton {
                required property var modelData
                appRoot: toolbarRoot.appRoot
                label: modelData[0]
                tip: modelData[2]
                active: appRoot.styleActive(modelData[1])
                onClicked: appRoot.applyStyle(modelData[1])
            }
        }

        ToolSeparator { appRoot: toolbarRoot.appRoot }

        ToolButton {
            appRoot: toolbarRoot.appRoot
            label: "•"; tip: "Bullet list"
            active: appRoot.formattingState.DefaultBullet === true
            onClicked: appRoot.applyFormat(".uno:DefaultBullet")
        }
        ToolButton {
            appRoot: toolbarRoot.appRoot
            label: "1."; tip: "Numbered list"
            active: appRoot.formattingState.DefaultNumbering === true
            onClicked: appRoot.applyFormat(".uno:DefaultNumbering")
        }

        ToolSeparator { appRoot: toolbarRoot.appRoot }

        Repeater {
            model: [["L", "LeftPara", "Align left"], ["C", "CenterPara", "Align center"],
                    ["R", "RightPara", "Align right"]]
            ToolButton {
                required property var modelData
                appRoot: toolbarRoot.appRoot
                label: modelData[0]
                tip: modelData[2]
                active: appRoot.formattingState[modelData[1]] === true
                onClicked: appRoot.applyFormat(".uno:" + modelData[1])
            }
        }

        ToolSeparator { appRoot: toolbarRoot.appRoot }

        // Paragraph spacing: directional only. LOK does not apply exact
        // point values or keep-together/widow-orphan headlessly (see
        // engine/README.md and the `para` command's comment) — these
        // buttons use the LO-native spacing step instead of a fixed pt.
        ToolButton {
            appRoot: toolbarRoot.appRoot
            label: "¶−"; tip: "Decrease paragraph spacing"
            onClicked: appRoot.applyParaSpacing("decrease")
        }
        ToolButton {
            appRoot: toolbarRoot.appRoot
            label: "¶+"; tip: "Increase paragraph spacing"
            onClicked: appRoot.applyParaSpacing("increase")
        }

        ToolSeparator { appRoot: toolbarRoot.appRoot }

        ToolButton {
            appRoot: toolbarRoot.appRoot
            fixedWidth: 130
            label: (appRoot.formattingState.CharFontName || "Font") + " ▾"
            tip: "Font: click for next (" + appRoot.fontNames.join(", ") + ")"
            border.color: Qt.alpha(appRoot.theme.foreground, 0.2)
            border.width: 1
            onClicked: appRoot.cycleFont()
        }

        ToolButton {
            appRoot: toolbarRoot.appRoot
            label: "−"; tip: "Decrease font size"
            onClicked: appRoot.stepFontSize(-1)
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: 40
            horizontalAlignment: Text.AlignHCenter
            text: isNaN(toolbarRoot.appRoot.currentFontSize) ? "—" : toolbarRoot.appRoot.currentFontSize + "pt"
            color: toolbarRoot.appRoot.theme.foreground
            opacity: toolbarRoot.appRoot.docId >= 0 ? 1 : 0.5
            font.pixelSize: 13
        }
        ToolButton {
            appRoot: toolbarRoot.appRoot
            label: "+"; tip: "Increase font size"
            onClicked: appRoot.stepFontSize(1)
        }

        ToolSeparator { appRoot: toolbarRoot.appRoot }

        ToolButton {
            id: colorButton
            appRoot: toolbarRoot.appRoot
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

        ToolButton {
            appRoot: toolbarRoot.appRoot
            label: "🖌"
            tip: "Format painter: click to copy formatting once, double-click to keep painting (Esc stops)"
            active: appRoot.paintMode
            // A double-click arrives as clicked then doubleClicked; the
            // first click captures, the second makes the mode sticky.
            onClicked: appRoot.paintMode ? appRoot.exitPaintMode() : appRoot.startPaintMode(false)
            onDoubleClicked: appRoot.startPaintMode(true)
        }
    }

    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 1
        color: toolbarRoot.appRoot.theme.muted
    }
}
