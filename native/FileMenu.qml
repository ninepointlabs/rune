import QtQuick
import QtQuick.Controls.Basic

// The toolbar's first button: the Rune mark and "File", opening a menu with
// New, Open, Save, Save As, the Autosave toggle and the command palette.
// Each item runs the same Main.qml function as its keyboard shortcut (the
// Shortcut items in Main.qml), so the menu only shows the key as a hint
// rather than binding it a second time.
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

    ThemedMenu {
        id: menu
        objectName: "fileMenu"
        theme: fileMenu.theme
        y: fileMenu.height + 4

        MenuEntry {
            objectName: "newMenuItem"
            text: "New"; hint: "Ctrl+N"
            enabled: fileMenu.available
            onTriggered: fileMenu.appRoot.requestNew()
        }
        MenuEntry {
            objectName: "openMenuItem"
            text: "Open…"; hint: "Ctrl+O"
            enabled: fileMenu.available
            onTriggered: fileMenu.appRoot.requestOpen()
        }
        MenuDivider { theme: fileMenu.theme }
        MenuEntry {
            objectName: "saveMenuItem"
            text: "Save"; hint: "Ctrl+S"
            enabled: fileMenu.available
            onTriggered: fileMenu.appRoot.save()
        }
        MenuEntry {
            objectName: "saveAsMenuItem"
            text: "Save As…"; hint: "Ctrl+Shift+S"
            enabled: fileMenu.available
            onTriggered: fileMenu.appRoot.saveAs()
        }
        MenuDivider { theme: fileMenu.theme }
        MenuEntry {
            objectName: "autosaveMenuItem"
            text: "Autosave"; hint: fileMenu.appRoot.autosaveEnabled ? "✓" : ""
            onTriggered: fileMenu.appRoot.autosaveEnabled = !fileMenu.appRoot.autosaveEnabled
        }
        MenuDivider { theme: fileMenu.theme }
        MenuEntry {
            objectName: "commandPaletteMenuItem"
            text: "Command palette…"; hint: "Ctrl+K"
            onTriggered: fileMenu.appRoot.openCommandPalette()
        }
    }
}
