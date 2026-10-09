import QtQuick

// A toolbar button: shows `label`, highlighted while `active`, with
// `tip` shown below it on hover. Same look as app/ToolButton.qml.
Rectangle {
    id: button
    required property var appRoot   // the Main.qml Window instance
    property string label
    property string tip
    property bool active: false
    property bool bold: false
    property bool italic: false
    property bool underline: false
    signal clicked()

    width: Math.max(28, buttonText.implicitWidth + 14)
    height: 28
    radius: 4
    color: active ? appRoot.theme.accent : mouse.containsMouse ? Qt.alpha(appRoot.theme.foreground, 0.1) : "transparent"

    Text {
        id: buttonText
        anchors { verticalCenter: parent.verticalCenter; left: parent.left; right: parent.right; margins: 7 }
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        text: button.label
        color: button.active ? button.appRoot.theme.background : button.appRoot.theme.foreground
        font.pixelSize: 13
        font.bold: button.bold
        font.italic: button.italic
        font.underline: button.underline
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }

    Rectangle {
        id: tooltip
        visible: button.tip !== "" && mouse.containsMouse && tipDelay.elapsed
        // Below the button, centered, kept inside the window.
        x: {
            const want = (button.width - width) / 2
            const left = button.mapToItem(null, 0, 0).x
            return Math.max(-left + 4, Math.min(want, button.appRoot.width - left - width - 4))
        }
        y: button.height + 6
        z: 100
        width: tipText.implicitWidth + 12
        height: tipText.implicitHeight + 6
        radius: 3
        color: button.appRoot.theme.background
        border.color: button.appRoot.theme.muted
        border.width: 1

        Text {
            id: tipText
            anchors.centerIn: parent
            text: button.tip
            color: button.appRoot.theme.foreground
            font.pixelSize: 11
        }

        Timer {
            id: tipDelay
            property bool elapsed: false
            interval: 400
            running: mouse.containsMouse
            onRunningChanged: if (!running) elapsed = false
            onTriggered: elapsed = true
        }
    }
}
