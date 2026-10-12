import QtQuick
import QtQuick.Controls.Basic

// The toolbar's first button: the Rune mark and "File", opening a menu with
// New, Open, Save, Save As and the Autosave toggle. Each item runs the same
// Main.qml function as its keyboard shortcut (the Shortcut items in
// Main.qml), so the menu only shows the key as a hint rather than binding
// it a second time.
Rectangle {
    id: fileMenu

    required property var appRoot   // the Main.qml Window instance
    readonly property var theme: appRoot.theme
    // Open/save are asynchronous; nothing else may start while one runs.
    property bool available: true

    // Alt+F (Main.qml), so the menu can be used from the keyboard.
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
        spacing: 6

        // The Rune mark, as on the site and the app icon.
        Canvas {
            id: mark
            width: 16
            height: 16
            anchors.verticalCenter: parent.verticalCenter
            onPaint: {
                const ctx = getContext("2d")
                const s = width / 100
                ctx.reset()
                ctx.strokeStyle = fileMenu.theme.accent
                ctx.lineWidth = 9 * s
                ctx.lineCap = "square"
                ctx.beginPath()
                ctx.moveTo(30 * s, 15 * s); ctx.lineTo(30 * s, 85 * s)
                ctx.moveTo(30 * s, 15 * s); ctx.lineTo(68 * s, 38 * s); ctx.lineTo(30 * s, 56 * s)
                ctx.moveTo(68 * s, 38 * s); ctx.lineTo(78 * s, 85 * s)
                ctx.stroke()
            }
            Connections {
                target: fileMenu
                function onThemeChanged() { mark.requestPaint() }
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "File ▾"
            color: fileMenu.theme.foreground
            font.pixelSize: 13
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: menu.visible ? menu.close() : menu.open()
    }

    component Entry: MenuItem {
        id: item
        property string hint   // the keyboard shortcut, shown right-aligned
        implicitWidth: 220
        implicitHeight: 28
        contentItem: Item {
            Text {
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                text: item.text
                color: item.highlighted ? fileMenu.theme.background : fileMenu.theme.foreground
                font.pixelSize: 13
            }
            Text {
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: item.hint
                color: item.highlighted ? fileMenu.theme.background : Qt.alpha(fileMenu.theme.foreground, 0.6)
                font.pixelSize: 12
            }
        }
        background: Rectangle {
            radius: 3
            color: item.highlighted ? fileMenu.theme.accent : "transparent"
        }
        opacity: enabled ? 1 : 0.5
    }

    Menu {
        id: menu
        objectName: "fileMenu"
        y: fileMenu.height + 4
        padding: 4
        background: Rectangle {
            implicitWidth: 220
            color: fileMenu.theme.background
            border.color: fileMenu.theme.muted
            border.width: 1
            radius: 4
        }

        Entry {
            objectName: "newMenuItem"
            text: "New"; hint: "Ctrl+N"
            enabled: fileMenu.available
            onTriggered: fileMenu.appRoot.requestNew()
        }
        Entry {
            objectName: "openMenuItem"
            text: "Open…"; hint: "Ctrl+O"
            enabled: fileMenu.available
            onTriggered: fileMenu.appRoot.requestOpen()
        }
        MenuSeparator {
            contentItem: Rectangle {
                implicitHeight: 1
                color: Qt.alpha(fileMenu.theme.muted, 0.6)
            }
        }
        Entry {
            objectName: "saveMenuItem"
            text: "Save"; hint: "Ctrl+S"
            enabled: fileMenu.available
            onTriggered: fileMenu.appRoot.save()
        }
        Entry {
            objectName: "saveAsMenuItem"
            text: "Save As…"; hint: "Ctrl+Shift+S"
            enabled: fileMenu.available
            onTriggered: fileMenu.appRoot.saveAs()
        }
        MenuSeparator {
            contentItem: Rectangle {
                implicitHeight: 1
                color: Qt.alpha(fileMenu.theme.muted, 0.6)
            }
        }
        Entry {
            objectName: "autosaveMenuItem"
            text: "Autosave"; hint: fileMenu.appRoot.autosaveEnabled ? "✓" : ""
            onTriggered: fileMenu.appRoot.autosaveEnabled = !fileMenu.appRoot.autosaveEnabled
        }
    }
}
