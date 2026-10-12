import QtQuick
import QtQuick.Controls.Basic

// A toolbar button that opens a ThemedMenu: an optional `icon`, `label`
// and a caret. The menu's items are this item's children.
Rectangle {
    id: button

    required property var appRoot   // the Main.qml Window instance
    readonly property var theme: appRoot.theme
    property string label
    property Component icon: null
    property bool caret: true
    property alias menuName: menu.objectName
    property alias menu: menu
    default property alias entries: menu.contentData

    signal closed()

    function openMenu() {
        menu.open()
        menu.currentIndex = 0
    }

    width: row.implicitWidth + 14
    height: 28
    radius: 4
    color: menu.visible ? Qt.alpha(theme.foreground, 0.15)
         : mouse.containsMouse ? Qt.alpha(theme.foreground, 0.1) : "transparent"

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 5
        Loader {
            anchors.verticalCenter: parent.verticalCenter
            active: button.icon !== null
            sourceComponent: button.icon
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.label !== ""
            text: button.label
            color: button.theme.foreground
            font.pixelSize: 13
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.caret
            text: "▾"
            font.pixelSize: 9
            color: button.theme.foreground
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: menu.visible ? menu.close() : menu.open()
    }

    ThemedMenu {
        id: menu
        theme: button.theme
        y: button.height + 4
        onClosed: button.closed()
    }
}
