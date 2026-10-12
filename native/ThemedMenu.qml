import QtQuick
import QtQuick.Controls.Basic

// A Menu in the app's theme. Its items are MenuEntry and MenuDivider,
// which take their colors from `theme` here.
Menu {
    id: menu
    required property var theme
    padding: 4
    margins: 6 // kept inside the window, moved left if need be
    background: Rectangle {
        implicitWidth: 220
        color: menu.theme.background
        border.color: menu.theme.muted
        border.width: 1
        radius: 4
    }
}
