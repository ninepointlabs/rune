import QtQuick
import QtQuick.Controls.Basic

// A line between groups of MenuEntry items; takes no space when hidden.
// (Separators don't know their menu, so the theme is passed in.)
MenuSeparator {
    id: divider
    required property var theme
    implicitHeight: visible ? topPadding + bottomPadding + 1 : 0
    contentItem: Rectangle {
        implicitHeight: 1
        color: Qt.alpha(divider.theme.muted, 0.6)
    }
}
