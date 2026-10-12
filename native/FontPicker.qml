import QtQuick
import QtQuick.Controls.Basic
import Rune

// The toolbar's font button: the family at the cursor, written in that
// font, opening a list of the fonts installed on the system, each name
// written in its own font (symbol and non-Latin fonts in the UI font, see
// DocumentController::fontFamilies()). Typing filters the list; Up/Down and
// Enter pick from the keyboard.
Rectangle {
    id: picker

    required property var appRoot   // the Main.qml Window instance
    required property DocumentController controller
    readonly property var theme: appRoot.theme
    // The controller's currentFontFamily().
    property string currentFamily: ""
    // {family, preview} (DocumentController::fontFamilies()), read when the
    // list opens so fonts installed meanwhile show up.
    property var families: []
    readonly property var filtered: {
        const q = search.text.trim().toLowerCase()
        return q === "" ? families : families.filter(f => f.family.toLowerCase().includes(q))
    }

    signal closed()

    // Opens the font list, as clicking the button does (the command palette's Font…).
    function openList() {
        popup.open()
    }

    function choose(family) {
        controller.setFontFamily(family)
        popup.close()
    }

    width: 150
    height: 28
    radius: 4
    color: popup.visible ? Qt.alpha(theme.foreground, 0.15)
         : mouse.containsMouse ? Qt.alpha(theme.foreground, 0.1) : "transparent"

    Text {
        anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 7; right: caret.left; rightMargin: 4 }
        text: picker.currentFamily
        font.family: picker.currentFamily
        font.pixelSize: 13
        elide: Text.ElideRight
        color: picker.theme.foreground
    }
    Text {
        id: caret
        anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 7 }
        text: "▾"
        font.pixelSize: 9
        color: picker.theme.foreground
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: popup.visible ? popup.close() : popup.open()
    }

    Popup {
        id: popup
        objectName: "fontPopup"
        y: picker.height + 4
        margins: 6 // kept inside the window, moved left if need be
        width: 300
        padding: 6
        focus: true
        onOpened: {
            picker.families = picker.controller.fontFamilies()
            search.text = ""
            list.currentIndex = Math.max(0, picker.filtered.findIndex(f => f.family === picker.currentFamily))
            list.positionViewAtIndex(list.currentIndex, ListView.Center)
            search.forceActiveFocus()
        }
        onClosed: picker.closed()
        background: Rectangle {
            color: picker.theme.background
            border.color: picker.theme.muted
            border.width: 1
            radius: 4
        }

        Column {
            width: parent.width
            spacing: 6

            TextField {
                id: search
                objectName: "fontSearch"
                width: parent.width
                height: 28
                placeholderText: "Search fonts"
                font.pixelSize: 13
                color: picker.theme.foreground
                placeholderTextColor: Qt.alpha(picker.theme.foreground, 0.5)
                background: Rectangle {
                    color: "transparent"
                    border.color: search.activeFocus ? picker.theme.accent : picker.theme.muted
                    border.width: 1
                    radius: 3
                }
                onTextChanged: list.currentIndex = 0
                Keys.onDownPressed: list.incrementCurrentIndex()
                Keys.onUpPressed: list.decrementCurrentIndex()
                onAccepted: if (list.currentIndex >= 0 && list.currentIndex < list.count)
                                picker.choose(picker.filtered[list.currentIndex].family)
            }

            ListView {
                id: list
                objectName: "fontList"
                width: parent.width
                height: 320
                clip: true
                model: picker.filtered
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
                    color: ListView.isCurrentItem ? Qt.alpha(picker.theme.accent, 0.25)
                         : rowMouse.containsMouse ? Qt.alpha(picker.theme.foreground, 0.1) : "transparent"

                    Text {
                        anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 8; right: check.left; rightMargin: 6 }
                        text: row.modelData.family
                        font.family: row.modelData.preview ? row.modelData.family : Qt.application.font.family
                        font.pixelSize: 15
                        elide: Text.ElideRight
                        color: picker.theme.foreground
                    }
                    Text {
                        id: check
                        anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 8 }
                        text: row.modelData.family === picker.currentFamily ? "✓" : ""
                        font.pixelSize: 12
                        color: picker.theme.accent
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: picker.choose(row.modelData.family)
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: list.count === 0
                    text: "No fonts match"
                    color: Qt.alpha(picker.theme.foreground, 0.6)
                    font.pixelSize: 12
                }
            }
        }
    }
}
