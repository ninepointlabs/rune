import QtQuick
import QtQuick.Controls.Basic
import Rune

// Story notes: the author's characters, places and research, saved in the
// document (see StoryNotes). Agents read and update the same notes.
Item {
    id: panel

    required property var appRoot   // the Main.qml Window instance
    required property DocumentController controller
    readonly property var theme: appRoot.theme

    readonly property var kinds: [["character", "Characters", "character"], ["place", "Places", "place"],
                                  ["research", "Research", "note"]]
    property string kind: "character"
    // The note being edited: its id, or "new" for one being added; "" none.
    property string editing: ""
    readonly property var shown: controller.notes.filter(n => n.kind === kind)

    // Adds a note of `noteKind` (opening its editor).
    function add(noteKind) {
        kind = noteKind
        editing = "new"
        editor.name = ""
        editor.text = ""
        editor.focusName()
    }

    function edit(note) {
        editing = note.id
        editor.name = note.name
        editor.text = note.text || ""
    }

    function save() {
        if (editor.name.trim() === "")
            return
        if (editing === "new")
            controller.addNote(kind, editor.name, editor.text)
        else
            controller.updateNote(editing, editor.name, editor.text)
        editing = ""
    }

    Row {
        id: kindTabs
        anchors { left: parent.left; top: parent.top }
        spacing: 4
        Repeater {
            model: panel.kinds
            PanelButton {
                required property var modelData
                theme: panel.theme
                objectName: "notesKind_" + modelData[0]
                label: modelData[1] + " " + panel.controller.notes.filter(n => n.kind === modelData[0]).length
                primary: panel.kind === modelData[0]
                onClicked: {
                    panel.kind = modelData[0]
                    panel.editing = ""
                }
            }
        }
    }

    PanelButton {
        id: addButton
        objectName: "addNote"
        anchors { left: parent.left; top: kindTabs.bottom; topMargin: 10 }
        theme: panel.theme
        label: "+ Add " + panel.kinds.find(k => k[0] === panel.kind)[2]
        onClicked: panel.add(panel.kind)
    }

    // Name and text of the note being edited.
    Column {
        id: editor
        objectName: "noteEditor"
        anchors { left: parent.left; right: parent.right; top: addButton.bottom; topMargin: 10 }
        visible: panel.editing !== ""
        spacing: 8
        property alias name: nameField.text
        property alias text: textField.text
        function focusName() { nameField.input.forceActiveFocus() }

        PanelField {
            id: nameField
            objectName: "noteName"
            width: parent.width
            theme: panel.theme
            label: "Name"
            onAccepted: textField.input.forceActiveFocus()
        }
        PanelArea {
            id: textField
            objectName: "noteText"
            width: parent.width
            theme: panel.theme
            label: panel.kind === "research" ? "Notes" : "Description"
            minimumHeight: 110
        }
        Row {
            spacing: 8
            PanelButton {
                objectName: "saveNote"
                theme: panel.theme
                label: "Save"
                primary: true
                available: nameField.text.trim() !== ""
                onClicked: panel.save()
            }
            PanelButton {
                theme: panel.theme
                label: "Cancel"
                onClicked: panel.editing = ""
            }
            PanelButton {
                objectName: "deleteNote"
                visible: panel.editing !== "new"
                theme: panel.theme
                label: "Delete"
                onClicked: {
                    panel.controller.removeNote(panel.editing)
                    panel.editing = ""
                }
            }
        }
    }

    Text {
        anchors { left: parent.left; right: parent.right; top: addButton.bottom; topMargin: 14 }
        visible: list.count === 0 && panel.editing === ""
        wrapMode: Text.WordWrap
        text: panel.kind === "research"
              ? "Research notes, facts and sources to remember. Saved in the document."
              : "Who and where your story is about: names and what to remember about them. Saved in the "
                + "document; the AI reads them, and updates them when it writes."
        color: Qt.alpha(panel.theme.foreground, 0.55)
        font.pixelSize: 12
    }

    ListView {
        id: list
        objectName: "notesList"
        anchors { left: parent.left; right: parent.right; top: editor.visible ? editor.bottom : addButton.bottom
                  bottom: parent.bottom; topMargin: 10 }
        clip: true
        spacing: 4
        model: panel.shown
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        delegate: Rectangle {
            id: card
            required property var modelData
            width: ListView.view.width
            height: cardColumn.implicitHeight + 12
            radius: 4
            color: cardMouse.containsMouse || panel.editing === modelData.id ? Qt.alpha(panel.theme.foreground, 0.08)
                                                                              : Qt.alpha(panel.theme.foreground, 0.03)
            Column {
                id: cardColumn
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 6 }
                spacing: 2
                Text {
                    width: parent.width
                    text: card.modelData.name
                    elide: Text.ElideRight
                    color: panel.theme.foreground
                    font.pixelSize: 13
                    font.bold: true
                }
                Text {
                    width: parent.width
                    visible: text !== ""
                    text: card.modelData.text || ""
                    wrapMode: Text.WordWrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    color: Qt.alpha(panel.theme.foreground, 0.7)
                    font.pixelSize: 12
                }
            }
            MouseArea {
                id: cardMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: panel.edit(card.modelData)
            }
        }
    }
}
