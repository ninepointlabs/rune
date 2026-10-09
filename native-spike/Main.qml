// native-spike: content-fidelity check for TextEdit (QTextDocument) as the
// editing core. Deliberately unthemed: light background, no Omarchy palette.
//
// Images are out of scope for this pass: <img> + QTextImageFormat is assumed
// to work per Qt documentation and is lower-risk than tables/lists.

import QtQuick

Window {
    id: root
    width: 800
    height: 1000
    visible: true
    color: "#ffffff"
    title: "Rune native spike"

    property bool autoExport: false
    readonly property string exportPath: "/tmp/native-spike-output.odt"

    function exportOdf() {
        const ok = exporter.exportToOdf(editor.textDocument, exportPath)
        console.log("export result:", ok, exportPath)
        return ok
    }

    // Explicit background: contentItem.grabToImage() ignores Window.color.
    Rectangle { anchors.fill: parent; color: root.color }

    OdfExporter { id: exporter }

    Shortcut {
        sequence: "Ctrl+S"
        onActivated: root.exportOdf()
    }

    Rectangle {
        id: exportButton
        anchors { top: parent.top; right: parent.right; margins: 8 }
        width: label.implicitWidth + 24
        height: 32
        radius: 4
        color: mouse.pressed ? "#c8c8c8" : "#e4e4e4"
        border.color: "#a0a0a0"

        Text { id: label; anchors.centerIn: parent; text: "Export to ODF (Ctrl+S)" }
        MouseArea { id: mouse; anchors.fill: parent; onClicked: root.exportOdf() }
    }

    Flickable {
        id: flick
        anchors { top: exportButton.bottom; left: parent.left; right: parent.right; bottom: parent.bottom }
        contentWidth: width
        contentHeight: editor.implicitHeight
        clip: true

        TextEdit {
            id: editor
            width: flick.width
            padding: 32
            focus: true
            selectByMouse: true
            persistentSelection: true
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            color: "#1a1a1a"
            font.pointSize: 11
            text: "<h1>Native Rich-Text Spike</h1>"
                + "<p>This paragraph has <b>bold text</b>, <i>italic text</i>, and "
                + "<u>underlined text</u> rendered by QTextDocument.</p>"
                + "<ul>"
                + "<li>First bullet item</li>"
                + "<li>Second bullet item"
                + "<ul><li>Nested bullet item</li></ul>"
                + "</li>"
                + "<li>Third bullet item</li>"
                + "</ul>"
                + "<table border=\"1\" cellspacing=\"0\" cellpadding=\"6\">"
                + "<thead><tr><th>Name</th><th>Role</th></tr></thead>"
                + "<tr><td>Ada</td><td>Engine</td></tr>"
                + "<tr><td>Grace</td><td>Compiler</td></tr>"
                + "</table>"
                + "<p>Closing paragraph after the table.</p>"
        }
    }

    // Headless verification: dump structure, screenshot, export, quit.
    Timer {
        running: root.autoExport
        interval: 500
        onTriggered: {
            console.log("document structure:\n" + exporter.describe(editor.textDocument))
            root.contentItem.grabToImage(result => {
                console.log("screenshot saved:", result.saveToFile("/tmp/native-spike-screenshot.png"))
                Qt.exit(root.exportOdf() ? 0 : 2)
            })
        }
    }
}
