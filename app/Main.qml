import QtQuick
import QtQuick.Window
import Rune

Window {
    id: root

    required property var theme
    required property string socketPath
    required property string documentPath

    // Pixels per CSS pixel for rendered tiles; the Image downscales to fit.
    readonly property real renderScale: 1.5

    property int docId: -1
    property int pageCount: 0
    property var pageRect: []
    property string tileSource: ""
    property string errorText: ""
    property bool loading: false

    readonly property string documentName: documentPath.substring(documentPath.lastIndexOf("/") + 1)

    width: 1000
    height: 1100
    visible: true
    title: documentName ? documentName + " — Rune" : "Rune"
    color: theme.background

    BridgeSocket {
        id: bridge
        socketPath: root.socketPath
        onConnectedChanged: {
            if (connected) {
                root.openDocument()
            } else {
                // Engine restarted or went away: its doc ids are gone too.
                root.docId = -1
                root.loading = false
            }
        }
    }

    Component.onCompleted: bridge.connectToEngine()

    function openDocument() {
        errorText = ""
        loading = true
        bridge.send({ cmd: "open", path: documentPath }, function (r) {
            if (!r.ok) {
                loading = false
                errorText = "Could not open " + documentName + ": " + r.error
                return
            }
            docId = r.doc_id
            pageCount = r.pages
            pageRect = r.page_rect
            requestFirstPage()
        })
    }

    function requestFirstPage() {
        bridge.send({
            cmd: "tile", doc_id: docId, part: 0,
            x: pageRect[0], y: pageRect[1], width: pageRect[2], height: pageRect[3],
            // 1440 twips/inch, 96 px/inch -> 15 twips per CSS pixel.
            px_width: Math.round(pageRect[2] / 15 * renderScale)
        }, function (r) {
            loading = false
            if (!r.ok) {
                errorText = "Render failed: " + r.error
                return
            }
            tileSource = "data:image/png;base64," + r.tile
        })
    }

    Flickable {
        id: view
        anchors { fill: parent; bottomMargin: statusBar.height }
        contentWidth: Math.max(width, page.width + 64)
        contentHeight: page.height + 64
        clip: true

        Rectangle {
            anchors.fill: page
            anchors.margins: -1
            visible: page.status === Image.Ready
            color: "transparent"
            border.color: theme.muted
            border.width: 1
        }

        Image {
            id: page
            x: (view.contentWidth - width) / 2
            y: 32
            source: root.tileSource
            asynchronous: true
            cache: false
            smooth: true
            mipmap: true
            fillMode: Image.PreserveAspectFit
            width: Math.min(sourceSize.width, view.width - 64)
            height: sourceSize.width > 0 ? width * sourceSize.height / sourceSize.width : 0
        }
    }

    // Centered message while there is nothing to show (or something broke).
    Text {
        visible: page.status !== Image.Ready || root.errorText !== ""
        anchors.centerIn: view
        width: parent.width * 0.8
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: 16
        color: root.errorText !== "" ? theme.red : theme.foreground
        text: root.errorText !== "" ? root.errorText
            : !bridge.connected ? (bridge.errorString || "Connecting to engine…") + "\nRetrying…"
            : root.loading ? "Rendering " + root.documentName + "…"
            : ""
    }

    Rectangle {
        id: statusBar
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 28
        color: theme.lighter_background !== undefined ? theme.lighter_background : theme.background

        Rectangle {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 1
            color: theme.accent
        }

        Text {
            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 10
                      right: connection.left; rightMargin: 16 }
            color: theme.foreground
            font.family: "monospace"
            font.pixelSize: 12
            elide: Text.ElideRight
            text: root.documentName
                  + (root.docId >= 0 ? "  ·  Page 1 of " + root.pageCount : "")
        }

        Row {
            id: connection
            anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 10 }
            spacing: 6

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 8; height: 8; radius: 4
                color: bridge.connected ? (theme.green ?? theme.accent)
                     : bridge.errorString ? theme.red
                     : (theme.yellow ?? theme.muted)
            }
            Text {
                color: theme.foreground
                font.family: "monospace"
                font.pixelSize: 12
                text: bridge.connected ? "engine connected"
                    : bridge.errorString ? "engine offline" : "connecting…"
            }
        }
    }
}
