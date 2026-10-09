import QtQuick
import QtQuick.Window

Window {
    id: root
    width: 1000
    height: 1100
    visible: true
    title: "Rune spike — LOK tile"
    color: theme.background

    Flickable {
        id: view
        anchors { fill: parent; bottomMargin: statusBar.height }
        contentWidth: Math.max(width, page.width + 64)
        contentHeight: page.height + 64
        clip: true

        // Drop shadow-ish border in the theme accent, Omarchy style.
        Rectangle {
            anchors.fill: page
            anchors.margins: -1
            color: "transparent"
            border.color: theme.muted
            border.width: 1
        }

        Image {
            id: page
            x: (view.contentWidth - width) / 2
            y: 32
            source: renderOk ? "image://lok/page0" : ""
            cache: false
            smooth: true
            // Fit the page to the window width; the tile itself is rendered
            // at --scale so downscaling stays crisp.
            fillMode: Image.PreserveAspectFit
            width: Math.min(sourceSize.width, view.width - 64)
            height: sourceSize.width > 0 ? width * sourceSize.height / sourceSize.width : 0
        }
    }

    Text {
        visible: !renderOk
        anchors.centerIn: parent
        width: parent.width * 0.8
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        color: theme.red
        font.pixelSize: 16
        text: statusText
    }

    Rectangle {
        id: statusBar
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 28
        color: theme.lighter_background !== undefined ? theme.lighter_background : theme.background

        Rectangle {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 1
            color: theme.accent
        }

        Text {
            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 10 }
            color: theme.foreground
            font.family: "monospace"
            font.pixelSize: 12
            text: statusText
            elide: Text.ElideRight
            width: parent.width - 20
        }
    }
}
