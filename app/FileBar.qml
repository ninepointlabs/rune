import QtQuick

// File actions, above the formatting toolbar.
Rectangle {
    id: fileBarRoot
    required property var appRoot   // the Main.qml Window instance

    height: 32
    z: 3 // tooltips overlap the toolbar below
    color: appRoot.theme.lighter_background !== undefined ? appRoot.theme.lighter_background : appRoot.theme.background

    Row {
        anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 4 }
        spacing: 4

        ToolButton {
            appRoot: fileBarRoot.appRoot
            label: "+ New"; tip: "New document"
            alwaysEnabled: appRoot.bridge.connected
            onClicked: appRoot.newDocument()
        }
        ToolButton {
            appRoot: fileBarRoot.appRoot
            label: "Open"; tip: "Open document"
            alwaysEnabled: true
            onClicked: appRoot.flashStatus("File → Open not yet implemented")
        }
        ToolButton {
            appRoot: fileBarRoot.appRoot
            label: "Save"; tip: "Save (Ctrl+S)"
            onClicked: appRoot.save()
        }
    }

    ToolButton {
        appRoot: fileBarRoot.appRoot
        anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 4 }
        label: "Quit"; tip: "Quit Rune"
        alwaysEnabled: true
        onClicked: Qt.quit()
    }
}
