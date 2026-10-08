import QtQuick

Rectangle {
    required property var appRoot   // the Main.qml Window instance

    width: 1
    height: 20
    anchors.verticalCenter: parent.verticalCenter
    color: appRoot.theme.muted
}
