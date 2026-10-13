import QtQuick

// A side panel's button, as the AI panel's: `primary` filled with the
// accent, otherwise outlined.
Rectangle {
    id: button
    required property var theme
    property string label
    property bool primary: false
    property bool available: true
    signal clicked()

    implicitWidth: buttonLabel.implicitWidth + 20
    implicitHeight: 28
    radius: 4
    opacity: available ? 1 : 0.5
    color: primary ? (mouse.containsMouse ? Qt.lighter(theme.accent, 1.1) : theme.accent)
                   : (mouse.containsMouse ? Qt.alpha(theme.foreground, 0.12) : Qt.alpha(theme.foreground, 0.06))
    border.color: primary ? "transparent" : Qt.alpha(theme.foreground, 0.2)

    // For tests and keyboard use: what a click does.
    function click() {
        if (available)
            clicked()
    }

    Text {
        id: buttonLabel
        anchors.centerIn: parent
        text: button.label
        color: button.primary ? button.theme.background : button.theme.foreground
        font.pixelSize: 12
        font.bold: button.primary
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        enabled: button.available
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
