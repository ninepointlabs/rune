import QtQuick

// Four lines of text set left, centered or right: `alignment` is
// Qt.AlignLeft, Qt.AlignHCenter or Qt.AlignRight.
Item {
    id: icon
    property int alignment: Qt.AlignLeft
    property color color: "white"
    implicitWidth: 16
    implicitHeight: 14

    Repeater {
        model: [1, 0.6, 0.85, 0.5] // line lengths, as a fraction of the width
        Rectangle {
            required property real modelData
            required property int index
            width: icon.width * modelData
            height: 2
            radius: 1
            y: index * 4
            x: icon.alignment === Qt.AlignRight ? icon.width - width
             : icon.alignment === Qt.AlignHCenter ? (icon.width - width) / 2 : 0
            color: icon.color
        }
    }
}
