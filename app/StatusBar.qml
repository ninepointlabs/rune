import QtQuick

Rectangle {
    id: statusBarRoot
    required property var appRoot   // the Main.qml Window instance
    // Main.qml's flashStatus() restarts this.
    property alias flashAnim: flashAnim

    height: 28
    color: appRoot.theme.lighter_background !== undefined ? appRoot.theme.lighter_background : appRoot.theme.background

    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 1
        color: statusBarRoot.appRoot.theme.accent
    }

    Text {
        id: statusText
        anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 10
                  right: connection.left; rightMargin: 16 }
        color: statusBarRoot.appRoot.theme.foreground
        font.family: "monospace"
        font.pixelSize: 12
        elide: Text.ElideRight
        text: statusBarRoot.appRoot.statusFlash !== "" ? statusBarRoot.appRoot.statusFlash
              : statusBarRoot.appRoot.documentName + (statusBarRoot.appRoot.dirty ? " ●" : "")
                + (statusBarRoot.appRoot.docId >= 0 && statusBarRoot.appRoot.pageCount > 0
                   ? "  ·  Page " + (statusBarRoot.appRoot.currentPage + 1) + " of " + statusBarRoot.appRoot.pageCount : "")

        // Shows the flash for 2 s, then fades back to the document name.
        SequentialAnimation {
            id: flashAnim
            PropertyAction { target: statusText; property: "opacity"; value: 1 }
            PauseAnimation { duration: 2000 }
            NumberAnimation { target: statusText; property: "opacity"; to: 0; duration: 200 }
            ScriptAction { script: statusBarRoot.appRoot.statusFlash = "" }
            NumberAnimation { target: statusText; property: "opacity"; to: 1; duration: 200 }
        }
    }

    Row {
        id: connection
        anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 10 }
        spacing: 6

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 8; height: 8; radius: 4
            color: statusBarRoot.appRoot.bridge.connected ? (statusBarRoot.appRoot.theme.green ?? statusBarRoot.appRoot.theme.accent)
                 : statusBarRoot.appRoot.bridge.errorString ? statusBarRoot.appRoot.theme.red
                 : (statusBarRoot.appRoot.theme.yellow ?? statusBarRoot.appRoot.theme.muted)
        }
        Text {
            color: statusBarRoot.appRoot.theme.foreground
            font.family: "monospace"
            font.pixelSize: 12
            text: statusBarRoot.appRoot.bridge.connected ? "engine connected"
                : statusBarRoot.appRoot.bridge.errorString ? "engine offline" : "connecting…"
        }
    }
}
