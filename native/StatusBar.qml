import QtQuick
import Rune

// The status bar: what is happening (opening, saving, a brief message like
// "Saved"), else the document's name; and on the right, the page the cursor
// is on, or how to leave the header or footer being edited.
Rectangle {
    id: bar

    required property var appRoot   // the Main.qml Window instance
    required property DocumentController controller
    readonly property var theme: appRoot.theme

    height: 28
    color: appRoot.barColor

    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 1
        color: bar.theme.accent
    }

    Text {
        anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 10
                  right: wordIndicator.visible ? wordIndicator.left : pageIndicator.left; rightMargin: 10 }
        color: bar.theme.foreground
        font.family: "monospace"
        font.pixelSize: 12
        elide: Text.ElideRight
        text: bar.controller.busy ? bar.appRoot.busyText
              : bar.appRoot.statusFlash !== "" ? bar.appRoot.statusFlash
              : bar.appRoot.documentName + (bar.controller.dirty ? " ●" : "")
    }

    // The document's length.
    Text {
        id: wordIndicator
        objectName: "wordIndicator"
        anchors { verticalCenter: parent.verticalCenter; right: pageIndicator.left; rightMargin: 16 }
        visible: !bar.appRoot.partEditing
        color: Qt.alpha(bar.theme.foreground, 0.7)
        font.family: "monospace"
        font.pixelSize: 12
        text: bar.controller.wordCount.toLocaleString(Qt.locale(), "f", 0) + (bar.controller.wordCount === 1 ? " word" : " words")
    }

    // The page the cursor is on.
    Text {
        id: pageIndicator
        objectName: "pageIndicator"
        anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 10 }
        color: Qt.alpha(bar.theme.foreground, 0.7)
        font.family: "monospace"
        font.pixelSize: 12
        text: bar.appRoot.partEditing ? "Editing " + bar.appRoot.editingPart + " · Esc or click the page to return"
              : "Page " + bar.appRoot.currentPage() + " of " + bar.controller.pageCount
    }
}
