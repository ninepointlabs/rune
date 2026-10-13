import QtQuick
import Rune

// The author's side panel, on the left: the outline (chapters and scenes),
// story notes, and sources. Everything in it is saved in the document.
Rectangle {
    id: sidebar

    required property var appRoot   // the Main.qml Window instance
    required property DocumentController controller
    readonly property var theme: appRoot.theme
    // "outline", "notes" or "sources".
    property string tab: "outline"

    signal closeRequested()

    // Shows the panel on `name`.
    function show(name) {
        tab = name
        visible = true
    }

    color: appRoot.barColor

    Rectangle {
        anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
        width: 1
        color: Qt.alpha(sidebar.theme.muted, 0.5)
    }

    Row {
        id: tabs
        anchors { left: parent.left; top: parent.top; margins: 10 }
        spacing: 4
        Repeater {
            model: [["outline", "Outline"], ["notes", "Notes"], ["sources", "Sources"]]
            ToolButton {
                required property var modelData
                objectName: "sidebarTab_" + modelData[0]
                appRoot: sidebar.appRoot
                label: modelData[1]
                active: sidebar.tab === modelData[0]
                onClicked: sidebar.tab = modelData[0]
            }
        }
    }
    ToolButton {
        anchors { right: parent.right; top: parent.top; margins: 10 }
        appRoot: sidebar.appRoot
        label: "×"
        tip: "Close"
        onClicked: sidebar.closeRequested()
    }

    Item {
        anchors { left: parent.left; right: parent.right; top: tabs.bottom; bottom: parent.bottom
                  leftMargin: 12; rightMargin: 12; topMargin: 12; bottomMargin: 10 }
        OutlinePanel {
            anchors.fill: parent
            visible: sidebar.tab === "outline"
            appRoot: sidebar.appRoot
            controller: sidebar.controller
        }
        NotesPanel {
            id: notesPanel
            objectName: "notesPanel"
            anchors.fill: parent
            visible: sidebar.tab === "notes"
            appRoot: sidebar.appRoot
            controller: sidebar.controller
        }
        SourcesPanel {
            objectName: "sourcesPanel"
            anchors.fill: parent
            visible: sidebar.tab === "sources"
            appRoot: sidebar.appRoot
            controller: sidebar.controller
        }
    }

    // Opens the Notes tab adding a note of `kind`.
    function addNote(kind) {
        show("notes")
        notesPanel.add(kind)
    }
}
