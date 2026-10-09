import QtQuick
import QtQuick.Window
import RuneNative

Window {
    id: root

    required property var theme

    // Stage 1 has no save dialog; Ctrl+S always writes here.
    readonly property string savePath: "/tmp/rune-native-test.odt"
    readonly property color barColor: theme.lighter_background !== undefined ? theme.lighter_background : theme.background
    readonly property string documentName: controller.currentPath !== ""
        ? controller.currentPath.substring(controller.currentPath.lastIndexOf("/") + 1) : "Untitled"
    // Brief status-bar message ("Saved"); empty shows the document name.
    property string statusFlash: ""

    // Toolbar state, refreshed from the controller on formatChanged.
    property bool boldActive: false
    property bool italicActive: false
    property bool underlineActive: false

    function refreshFormat() {
        boldActive = controller.isBold()
        italicActive = controller.isItalic()
        underlineActive = controller.isUnderline()
    }

    function syncSelection() {
        controller.setSelection(editor.cursorPosition, editor.selectionStart, editor.selectionEnd)
    }

    function save() {
        statusFlash = controller.saveToOdf(savePath) ? "Saved " + savePath : "Save failed: " + savePath
        flashTimer.restart()
    }

    width: 1000
    height: 800
    visible: true
    color: theme.background
    title: documentName + (controller.dirty ? " ●" : "") + " — Rune"

    // Installs the controller's document into the TextEdit; see
    // DocumentController.h for why this isn't a binding.
    Component.onCompleted: {
        controller.document = editor.textDocument
        controller.newDocument()
        syncSelection()
    }

    DocumentController {
        id: controller
        objectName: "controller"
        onFormatChanged: root.refreshFormat()
    }

    Shortcut { sequence: StandardKey.Save; onActivated: root.save() }
    Shortcut { sequence: StandardKey.Bold; onActivated: controller.toggleBold() }
    Shortcut { sequence: StandardKey.Italic; onActivated: controller.toggleItalic() }
    Shortcut { sequence: StandardKey.Underline; onActivated: controller.toggleUnderline() }

    Rectangle {
        id: toolbar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 40
        z: 2 // tooltips overlap the document view
        color: root.barColor

        Row {
            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 4 }
            spacing: 4

            ToolButton {
                objectName: "boldButton"
                appRoot: root
                label: "B"; bold: true; tip: "Bold (Ctrl+B)"
                active: root.boldActive
                onClicked: controller.toggleBold()
            }
            ToolButton {
                appRoot: root
                label: "I"; italic: true; tip: "Italic (Ctrl+I)"
                active: root.italicActive
                onClicked: controller.toggleItalic()
            }
            ToolButton {
                appRoot: root
                label: "U"; underline: true; tip: "Underline (Ctrl+U)"
                active: root.underlineActive
                onClicked: controller.toggleUnderline()
            }
        }

        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1
            color: Qt.alpha(root.theme.muted, 0.5)
        }
    }

    Flickable {
        id: flick
        anchors { left: parent.left; right: parent.right; top: toolbar.bottom; bottom: statusBar.top }
        contentWidth: width
        contentHeight: editor.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        // Keep the cursor in view while typing.
        function ensureVisible(r) {
            if (contentY >= r.y)
                contentY = r.y
            else if (contentY + height <= r.y + r.height)
                contentY = r.y + r.height - height
        }

        TextEdit {
            id: editor
            objectName: "editor"
            width: flick.width
            height: Math.max(implicitHeight, flick.height)
            padding: 48
            focus: true
            selectByMouse: true
            persistentSelection: true
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            color: root.theme.foreground
            selectionColor: root.theme.accent
            selectedTextColor: root.theme.background
            font.pointSize: 12

            onCursorPositionChanged: root.syncSelection()
            onSelectionStartChanged: root.syncSelection()
            onSelectionEndChanged: root.syncSelection()
            onCursorRectangleChanged: flick.ensureVisible(cursorRectangle)
        }
    }

    Rectangle {
        id: statusBar
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 28
        color: root.barColor

        Rectangle {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 1
            color: root.theme.accent
        }

        Text {
            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 10
                      right: parent.right; rightMargin: 10 }
            color: root.theme.foreground
            font.family: "monospace"
            font.pixelSize: 12
            elide: Text.ElideRight
            text: root.statusFlash !== "" ? root.statusFlash
                  : root.documentName + (controller.dirty ? " ●" : "")
        }

        Timer { id: flashTimer; interval: 2500; onTriggered: root.statusFlash = "" }
    }
}
