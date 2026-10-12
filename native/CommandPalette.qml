import QtQuick
import QtQuick.Controls.Basic

// Ctrl+K: every command in one searchable list. Type to filter (each word
// must appear in the command's group or name), Up/Down to move, Enter or a
// click to run, Escape to close. Commands that don't apply right now (table
// edits outside a table, say) are listed dimmed and don't run.
//
// `commands` entries: {group, name, hint, available: () => bool, run: () => void}.
Popup {
    id: commandPalette // not `palette`: every Item has a property by that name

    required property var appRoot   // the Main.qml Window instance
    readonly property var theme: appRoot.theme
    property var commands: []
    // The matches for the search text, best first: [{command, available}].
    property var results: []

    function refresh() {
        const words = search.text.toLowerCase().split(/\s+/).filter(w => w !== "")
        const scored = []
        commands.forEach((command, order) => {
            const haystack = (command.group + " " + command.name).toLowerCase()
            let score = 0
            for (const word of words) {
                const at = haystack.indexOf(word)
                if (at < 0)
                    return
                // Matches at the start of a word rank first.
                score += at === 0 || haystack[at - 1] === " " ? 0 : 1
            }
            scored.push({command: command, available: command.available(), score: score, order: order})
        })
        scored.sort((a, b) => (b.available - a.available) || (a.score - b.score) || (a.order - b.order))
        results = scored
        list.currentIndex = 0
    }

    // Closes the palette, then runs the command (so a command that opens
    // another popup, like Font…, gets the focus).
    function runAt(index) {
        const result = results[index]
        if (!result || !result.available)
            return false
        close()
        Qt.callLater(result.command.run)
        return true
    }

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: 60
    width: Math.min(520, parent.width - 40)
    padding: 6
    modal: true
    focus: true
    onOpened: {
        search.text = ""
        refresh()
        search.forceActiveFocus()
    }
    Overlay.modal: Rectangle { color: Qt.alpha("black", 0.35) }
    background: Rectangle {
        color: commandPalette.theme.background
        border.color: commandPalette.theme.muted
        border.width: 1
        radius: 6
    }

    Column {
        width: parent.width
        spacing: 6

        TextField {
            id: search
            objectName: "paletteSearch"
            width: parent.width
            height: 34
            placeholderText: "Type a command"
            font.pixelSize: 14
            color: commandPalette.theme.foreground
            placeholderTextColor: Qt.alpha(commandPalette.theme.foreground, 0.5)
            background: Rectangle {
                color: "transparent"
                border.color: search.activeFocus ? commandPalette.theme.accent : commandPalette.theme.muted
                border.width: 1
                radius: 4
            }
            onTextChanged: commandPalette.refresh()
            Keys.onDownPressed: list.incrementCurrentIndex()
            Keys.onUpPressed: list.decrementCurrentIndex()
            onAccepted: commandPalette.runAt(list.currentIndex)
        }

        ListView {
            id: list
            objectName: "paletteList"
            width: parent.width
            height: Math.max(30, Math.min(contentHeight, 360))
            clip: true
            model: commandPalette.results
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                width: ListView.view.width
                height: 30
                radius: 3
                opacity: modelData.available ? 1 : 0.45
                color: ListView.isCurrentItem ? Qt.alpha(commandPalette.theme.accent, 0.25)
                     : rowMouse.containsMouse ? Qt.alpha(commandPalette.theme.foreground, 0.1) : "transparent"

                Row {
                    anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 8 }
                    spacing: 8
                    Text {
                        width: 52
                        text: row.modelData.command.group
                        color: Qt.alpha(commandPalette.theme.foreground, 0.55)
                        font.pixelSize: 12
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: row.modelData.command.name
                        color: commandPalette.theme.foreground
                        font.pixelSize: 14
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                Text {
                    anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 8 }
                    text: row.modelData.command.hint || ""
                    color: Qt.alpha(commandPalette.theme.foreground, 0.6)
                    font.pixelSize: 12
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: commandPalette.runAt(row.index)
                }
            }

            Text {
                anchors.centerIn: parent
                visible: list.count === 0
                text: "No matching commands"
                color: Qt.alpha(commandPalette.theme.foreground, 0.6)
                font.pixelSize: 12
            }
        }
    }
}
