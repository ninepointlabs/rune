import QtQuick
import QtQuick.Controls.Basic

// An item in a ThemedMenu: its text, and on the right `hint` (a keyboard
// shortcut, or "✓" for a current choice). A hidden entry takes no space,
// so a menu can show only the entries that apply.
MenuItem {
    id: item
    property string hint
    readonly property var theme: menu ? menu.theme : null

    implicitWidth: 220
    implicitHeight: visible ? 28 : 0
    opacity: enabled ? 1 : 0.5
    contentItem: Item {
        Text {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
            text: item.text
            color: item.theme ? (item.highlighted ? item.theme.background : item.theme.foreground) : "white"
            font.pixelSize: 13
        }
        Text {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            text: item.hint
            color: item.theme ? (item.highlighted ? item.theme.background : Qt.alpha(item.theme.foreground, 0.6)) : "white"
            font.pixelSize: 12
        }
    }
    background: Rectangle {
        radius: 3
        color: item.highlighted && item.theme ? item.theme.accent : "transparent"
    }
}
