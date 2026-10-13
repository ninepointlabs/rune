import QtQuick
import QtQuick.Controls.Basic
import Rune

// The outline: the document's chapters and scenes (Heading 1, Heading 2,
// ...) with their word counts. A click goes to one; dragging one by its
// handle moves the whole section, subsections and all, as one undo step.
Item {
    id: panel

    required property var appRoot   // the Main.qml Window instance
    required property DocumentController controller
    readonly property var theme: appRoot.theme

    // While a section is dragged: its index and where it would go (an index
    // to move before, or the outline's length for the end); -1 when not.
    property int dragFrom: -1
    property int dropAt: -1

    function words(n) {
        return n.toLocaleString(Qt.locale(), "f", 0) + (n === 1 ? " word" : " words")
    }

    Text {
        id: total
        objectName: "outlineWordCount"
        anchors { left: parent.left; right: parent.right; top: parent.top }
        text: panel.words(panel.controller.wordCount) + " in all"
        color: Qt.alpha(panel.theme.foreground, 0.7)
        font.pixelSize: 12
    }

    Text {
        anchors { left: parent.left; right: parent.right; top: total.bottom; topMargin: 16 }
        visible: list.count === 0
        wrapMode: Text.WordWrap
        text: "Chapters and scenes appear here. Make a paragraph a heading with the paragraph style menu "
              + "(Heading 1 for a chapter, Heading 2 for a scene), or Ctrl+Alt+1 and Ctrl+Alt+2."
        color: Qt.alpha(panel.theme.foreground, 0.55)
        font.pixelSize: 12
    }

    ListView {
        id: list
        objectName: "outlineList"
        anchors { left: parent.left; right: parent.right; top: total.bottom; bottom: parent.bottom; topMargin: 8 }
        clip: true
        model: panel.controller.outline
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        delegate: Rectangle {
            id: row
            required property var modelData
            required property int index
            width: ListView.view.width
            height: 30
            radius: 3
            color: rowMouse.containsMouse ? Qt.alpha(panel.theme.foreground, 0.08) : "transparent"
            opacity: panel.dragFrom === index ? 0.4 : 1

            // Where a drop would put the dragged section.
            Rectangle {
                visible: panel.dragFrom >= 0 && (panel.dropAt === row.index
                                                 || (panel.dropAt === list.count && row.index === list.count - 1))
                anchors { left: parent.left; right: parent.right }
                y: panel.dropAt === row.index ? 0 : parent.height - height
                height: 2
                color: panel.theme.accent
            }

            Text {
                id: handle
                anchors { left: parent.left; leftMargin: 2; verticalCenter: parent.verticalCenter }
                text: "⠿"
                color: Qt.alpha(panel.theme.foreground, handleMouse.containsMouse ? 0.8 : 0.35)
                font.pixelSize: 13
                MouseArea {
                    id: handleMouse
                    objectName: "outlineHandle"
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.SizeVerCursor
                    preventStealing: true
                    onPressed: {
                        panel.dragFrom = row.index
                        panel.dropAt = row.index
                    }
                    onPositionChanged: mouse => {
                        if (!pressed)
                            return
                        const p = mapToItem(list.contentItem, mouse.x, mouse.y)
                        const at = list.indexAt(10, p.y)
                        if (at < 0)
                            panel.dropAt = p.y < 0 ? 0 : list.count
                        else
                            panel.dropAt = p.y - list.itemAtIndex(at).y < 15 ? at : at + 1
                    }
                    onReleased: {
                        const from = panel.dragFrom, to = panel.dropAt
                        panel.dragFrom = panel.dropAt = -1
                        if (to >= 0 && to !== from && to !== from + 1)
                            panel.controller.moveSection(from, to)
                    }
                    onCanceled: panel.dragFrom = panel.dropAt = -1
                }
            }

            Text {
                anchors { left: handle.right; leftMargin: 6 + (row.modelData.level - 1) * 14
                          right: count.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
                text: row.modelData.title !== "" ? row.modelData.title : "Untitled"
                elide: Text.ElideRight
                color: panel.theme.foreground
                font.pixelSize: 13
                font.bold: row.modelData.level === 1
            }
            Text {
                id: count
                anchors { right: parent.right; rightMargin: 6; verticalCenter: parent.verticalCenter }
                text: row.modelData.words.toLocaleString(Qt.locale(), "f", 0)
                color: Qt.alpha(panel.theme.foreground, 0.5)
                font.pixelSize: 11
            }
            MouseArea {
                id: rowMouse
                anchors { left: handle.right; right: parent.right; top: parent.top; bottom: parent.bottom }
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: panel.appRoot.goToPosition(row.modelData.position)
            }
        }
    }
}
