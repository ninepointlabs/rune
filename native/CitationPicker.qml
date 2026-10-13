import QtQuick
import QtQuick.Controls.Basic
import Rune

// Insert citation (Ctrl+Shift+C): search the sources, pick one or more,
// add a page, insert.
Popup {
    id: picker

    required property var appRoot   // the Main.qml Window instance
    required property DocumentController controller
    readonly property var theme: appRoot.theme

    // Ids picked, in order.
    property var chosen: []
    readonly property var filtered: {
        const q = search.text.trim().toLowerCase()
        return controller.sources.filter(s => q === "" || s.label.toLowerCase().includes(q) || s.id.toLowerCase().includes(q))
    }

    function openPicker() {
        chosen = []
        search.text = ""
        page.text = ""
        list.currentIndex = 0
        open()
        search.input.forceActiveFocus()
    }

    function toggle(id) {
        const next = chosen.slice()
        const at = next.indexOf(id)
        if (at >= 0)
            next.splice(at, 1)
        else
            next.push(id)
        chosen = next
    }

    function insert() {
        let ids = chosen
        if (ids.length === 0 && list.currentIndex >= 0 && list.currentIndex < filtered.length)
            ids = [filtered[list.currentIndex].id]
        if (ids.length === 0)
            return
        close()
        controller.insertCitation(ids, page.text)
    }

    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: 460
    height: 420
    padding: 14
    background: Rectangle {
        color: picker.appRoot.barColor
        border.color: Qt.alpha(picker.theme.foreground, 0.25)
        radius: 6
    }
    onClosed: picker.appRoot.focusEditor()

    Column {
        anchors.fill: parent
        spacing: 8

        Text {
            text: "Insert citation"
            color: picker.theme.foreground
            font.pixelSize: 14
            font.bold: true
        }
        PanelField {
            id: search
            objectName: "citationSearch"
            width: parent.width
            theme: picker.theme
            placeholderText: "Search by author, title or key"
            input.Keys.onDownPressed: list.incrementCurrentIndex()
            input.Keys.onUpPressed: list.decrementCurrentIndex()
            onAccepted: picker.insert()
        }
        Rectangle {
            width: parent.width
            height: parent.height - y - bottomRow.height - 8
            color: picker.theme.background
            border.color: Qt.alpha(picker.theme.foreground, 0.15)
            radius: 3
            Text {
                anchors.centerIn: parent
                width: parent.width - 40
                visible: picker.controller.sources.length === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: "No sources yet. Add them in the Sources panel (Ctrl+K → Show sources)."
                color: Qt.alpha(picker.theme.foreground, 0.55)
                font.pixelSize: 12
            }
            ListView {
                id: list
                objectName: "citationList"
                anchors.fill: parent
                anchors.margins: 2
                clip: true
                model: picker.filtered
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 34
                    readonly property bool picked: picker.chosen.includes(modelData.id)
                    color: ListView.isCurrentItem ? Qt.alpha(picker.theme.accent, 0.25)
                         : rowMouse.containsMouse ? Qt.alpha(picker.theme.foreground, 0.08) : "transparent"
                    Text {
                        id: check
                        anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                        text: row.picked ? "☑" : "☐"
                        color: row.picked ? picker.theme.accent : Qt.alpha(picker.theme.foreground, 0.5)
                        font.pixelSize: 14
                    }
                    Text {
                        anchors { left: check.right; leftMargin: 8; right: parent.right; rightMargin: 8
                                  verticalCenter: parent.verticalCenter }
                        text: row.modelData.label + (row.modelData.unreviewed ? "  (unreviewed)" : "")
                        elide: Text.ElideRight
                        color: picker.theme.foreground
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            list.currentIndex = row.index
                            picker.toggle(row.modelData.id)
                        }
                        onDoubleClicked: {
                            picker.chosen = [row.modelData.id]
                            picker.insert()
                        }
                    }
                }
            }
        }
        Row {
            id: bottomRow
            width: parent.width
            spacing: 8
            PanelField {
                id: page
                objectName: "citationPage"
                width: 120
                theme: picker.theme
                placeholderText: "Page (optional)"
                onAccepted: picker.insert()
            }
            Item { width: parent.width - 120 - insertButton.width - cancelButton.width - 24; height: 1 }
            PanelButton {
                id: cancelButton
                anchors.verticalCenter: parent.verticalCenter
                theme: picker.theme
                label: "Cancel"
                onClicked: picker.close()
            }
            PanelButton {
                id: insertButton
                objectName: "citationInsert"
                anchors.verticalCenter: parent.verticalCenter
                theme: picker.theme
                label: picker.chosen.length > 1 ? "Cite " + picker.chosen.length + " sources" : "Insert"
                primary: true
                available: picker.filtered.length > 0 || picker.chosen.length > 0
                onClicked: picker.insert()
            }
        }
    }
}
