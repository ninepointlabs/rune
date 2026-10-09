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

        // Paragraph style dropdown; the popup reuses ToolButton's tooltip
        // pattern (a z:100 Rectangle below the control) rather than a ComboBox.
        ToolButton {
            id: styleButton
            appRoot: toolbarRoot.appRoot
            property bool open: false
            readonly property var styles: ["Normal", "Heading 1", "Heading 2", "Heading 3"]
            readonly property string current: styles.find(n => appRoot.styleActive(n)) || "Normal"
            fixedWidth: 110
            label: current + " ▾"
            tip: open ? "" : "Paragraph style"
            active: open
            border.color: Qt.alpha(appRoot.theme.foreground, 0.2)
            border.width: 1
            onClicked: open = !open
            onUsableChanged: if (!usable) open = false

            Rectangle {
                id: stylePopup
                visible: styleButton.open
                y: styleButton.height + 4
                z: 100
                width: styleButton.width
                height: styleList.implicitHeight + 8
                radius: 3
                color: styleButton.appRoot.theme.background
                border.color: styleButton.appRoot.theme.muted
                border.width: 1

                Column {
                    id: styleList
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 4 }

                    Repeater {
                        model: styleButton.styles
                        Rectangle {
                            id: styleItem
                            required property string modelData
                            readonly property bool current: styleButton.current === modelData
                            width: styleList.width
                            height: 24
                            radius: 3
                            color: current ? styleButton.appRoot.theme.accent
                                 : itemMouse.containsMouse ? Qt.alpha(styleButton.appRoot.theme.foreground, 0.1) : "transparent"

                            Text {
                                anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 7 }
                                text: styleItem.modelData
                                color: styleItem.current ? styleButton.appRoot.theme.background : styleButton.appRoot.theme.foreground
                                font.pixelSize: 13
                            }

                            MouseArea {
                                id: itemMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    styleButton.open = false
                                    styleButton.appRoot.applyStyle(styleItem.modelData)
                                }
                            }
                        }
                    }
                }
            }
        }

        ToolSeparator { appRoot: toolbarRoot.appRoot }

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
    }

    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 1
        color: toolbarRoot.appRoot.theme.muted
    }
}
