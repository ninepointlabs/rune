import QtQuick
import QtQuick.Controls.Basic

// A side panel's multi-line text field, with an optional label above it.
Column {
    id: area
    required property var theme
    property string label
    property alias text: input.text
    property alias placeholderText: input.placeholderText
    property alias input: input
    property real minimumHeight: 64

    spacing: 3
    Text {
        visible: area.label !== ""
        text: area.label
        color: Qt.alpha(area.theme.foreground, 0.7)
        font.pixelSize: 11
    }
    TextArea {
        id: input
        width: area.width
        height: Math.max(area.minimumHeight, implicitHeight)
        wrapMode: TextArea.Wrap
        font.pixelSize: 13
        color: area.theme.foreground
        selectionColor: area.theme.accent
        selectedTextColor: area.theme.background
        placeholderTextColor: Qt.alpha(area.theme.foreground, 0.4)
        background: Rectangle {
            color: area.theme.background
            border.color: input.activeFocus ? area.theme.accent : Qt.alpha(area.theme.foreground, 0.2)
            radius: 3
        }
    }
}
