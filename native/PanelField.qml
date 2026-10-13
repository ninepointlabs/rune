import QtQuick
import QtQuick.Controls.Basic

// A side panel's one-line text field, with an optional label above it.
Column {
    id: field
    required property var theme
    property string label
    property alias text: input.text
    property alias placeholderText: input.placeholderText
    property alias input: input
    signal accepted()

    spacing: 3
    Text {
        visible: field.label !== ""
        text: field.label
        color: Qt.alpha(field.theme.foreground, 0.7)
        font.pixelSize: 11
    }
    TextField {
        id: input
        width: field.width
        height: 28
        font.pixelSize: 13
        color: field.theme.foreground
        selectionColor: field.theme.accent
        selectedTextColor: field.theme.background
        placeholderTextColor: Qt.alpha(field.theme.foreground, 0.4)
        background: Rectangle {
            color: field.theme.background
            border.color: input.activeFocus ? field.theme.accent : Qt.alpha(field.theme.foreground, 0.2)
            radius: 3
        }
        onAccepted: field.accepted()
    }
}
