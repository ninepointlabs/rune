import QtQuick
import QtQuick.Controls.Basic
import Rune

// The toolbar's text-color button: "A" over a swatch of the color at the
// cursor, opening a palette. Automatic removes any explicit color; Custom
// takes a #RRGGBB value.
Rectangle {
    id: picker

    required property var appRoot   // the Main.qml Window instance
    required property DocumentController controller
    readonly property var theme: appRoot.theme
    // "#rrggbb", or "auto" (the controller's currentTextColor()).
    property string currentColor: "auto"

    // Rows of swatches: grays, then each hue bright, light, medium, dark,
    // darker (the palette word processors commonly use). Not `palette`:
    // every Item already has one (its Qt Quick Controls colors).
    readonly property var swatches: [
        ["#000000", "#434343", "#666666", "#999999", "#b7b7b7", "#cccccc", "#d9d9d9", "#efefef", "#f3f3f3", "#ffffff"],
        ["#980000", "#ff0000", "#ff9900", "#ffff00", "#00ff00", "#00ffff", "#4a86e8", "#0000ff", "#9900ff", "#ff00ff"],
        ["#ea9999", "#f9cb9c", "#ffe599", "#b6d7a8", "#a2c4c9", "#a4c2f4", "#9fc5e8", "#b4a7d6", "#d5a6bd", "#dd7e6b"],
        ["#e06666", "#f6b26b", "#ffd966", "#93c47d", "#76a5af", "#6d9eeb", "#6fa8dc", "#8e7cc3", "#c27ba0", "#cc4125"],
        ["#cc0000", "#e69138", "#f1c232", "#6aa84f", "#45818e", "#3c78d8", "#3d85c6", "#674ea7", "#a64d79", "#a61c00"],
        ["#990000", "#b45f06", "#bf9000", "#38761d", "#134f5c", "#1155cc", "#0b5394", "#351c75", "#741b47", "#85200c"]
    ]

    signal closed()

    function choose(color) {
        controller.setTextColor(color)
        popup.close()
    }

    // "#RRGGBB" or "RRGGBB"; false (doing nothing) for anything else.
    function chooseCustom(text) {
        const hex = text.trim().replace(/^#/, "")
        if (!/^[0-9a-fA-F]{6}$/.test(hex))
            return false
        choose("#" + hex.toLowerCase())
        return true
    }

    width: 40
    height: 28
    radius: 4
    color: popup.visible ? Qt.alpha(theme.foreground, 0.15)
         : mouse.containsMouse ? Qt.alpha(theme.foreground, 0.1) : "transparent"

    Text {
        anchors { horizontalCenter: parent.horizontalCenter; horizontalCenterOffset: -5; top: parent.top; topMargin: 3 }
        text: "A"
        font.bold: true
        font.pixelSize: 13
        color: picker.theme.foreground
    }
    Text {
        anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 5 }
        text: "▾"
        font.pixelSize: 9
        color: picker.theme.foreground
    }
    // Swatch of the color at the cursor; automatic shows as the foreground.
    Rectangle {
        anchors { horizontalCenter: parent.horizontalCenter; horizontalCenterOffset: -5; bottom: parent.bottom; bottomMargin: 4 }
        width: 16; height: 3; radius: 1
        color: picker.currentColor === "auto" ? picker.theme.foreground : picker.currentColor
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: popup.visible ? popup.close() : popup.open()
    }

    Popup {
        id: popup
        objectName: "colorPopup"
        y: picker.height + 4
        margins: 6 // kept inside the window, moved left if need be
        padding: 8
        focus: true
        onOpened: customField.text = picker.currentColor === "auto" ? "" : picker.currentColor
        onClosed: picker.closed()
        background: Rectangle {
            color: picker.theme.background
            border.color: picker.theme.muted
            border.width: 1
            radius: 4
        }

        Column {
            spacing: 6

            // Automatic: no explicit color, so the text follows the theme.
            Rectangle {
                objectName: "colorAutomatic"
                width: grid.width
                height: 26
                radius: 3
                color: autoMouse.containsMouse ? Qt.alpha(picker.theme.foreground, 0.1) : "transparent"
                border.color: picker.currentColor === "auto" ? picker.theme.accent : "transparent"
                border.width: 1
                Row {
                    anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 6 }
                    spacing: 8
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16; height: 16; radius: 2
                        color: picker.theme.foreground
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Automatic"
                        color: picker.theme.foreground
                        font.pixelSize: 13
                    }
                }
                MouseArea {
                    id: autoMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: picker.choose("auto")
                }
            }

            Grid {
                id: grid
                columns: 10
                spacing: 4

                Repeater {
                    model: [].concat(...picker.swatches)
                    delegate: Rectangle {
                        id: swatch
                        required property string modelData
                        objectName: "swatch" + modelData
                        readonly property bool current: picker.currentColor === modelData
                        width: 20; height: 20; radius: 3
                        color: modelData
                        // A ring around the current color, and on hover.
                        border.width: current || swatchMouse.containsMouse ? 2 : 1
                        border.color: current ? picker.theme.accent
                                    : swatchMouse.containsMouse ? picker.theme.foreground
                                    : Qt.alpha(picker.theme.foreground, 0.2)
                        MouseArea {
                            id: swatchMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: picker.choose(swatch.modelData)
                        }
                    }
                }
            }

            Row {
                spacing: 6
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Custom"
                    color: picker.theme.foreground
                    font.pixelSize: 12
                }
                TextField {
                    id: customField
                    objectName: "colorCustomField"
                    width: grid.width - 52 - 6 - 6 - 40
                    height: 26
                    placeholderText: "#RRGGBB"
                    font.pixelSize: 12
                    color: picker.theme.foreground
                    placeholderTextColor: Qt.alpha(picker.theme.foreground, 0.5)
                    background: Rectangle {
                        color: "transparent"
                        border.color: customField.activeFocus ? picker.theme.accent : picker.theme.muted
                        border.width: 1
                        radius: 3
                    }
                    onAccepted: picker.chooseCustom(text)
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 40; height: 26; radius: 3
                    readonly property bool valid: /^#?[0-9a-fA-F]{6}$/.test(customField.text.trim())
                    color: valid ? "#" + customField.text.trim().replace(/^#/, "") : "transparent"
                    border.color: picker.theme.muted
                    border.width: 1
                    MouseArea {
                        anchors.fill: parent
                        onClicked: picker.chooseCustom(customField.text)
                    }
                }
            }
        }
    }
}
