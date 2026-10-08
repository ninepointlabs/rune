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
    // Set once the first render lands; re-renders keep the old image up.
    property bool pageShown: false
    property bool renderInFlight: false
    property bool renderQueued: false

    // Twips (document coords) -> pixels on the displayed page.
    readonly property real twipsScale: pageRect.length === 4 && pageRect[2] > 0
                                       ? page.width / pageRect[2] : 0
    // Cursor and selection as last reported by the engine, in twips
    // [x, y, w, h]; the overlays convert so they follow window resizes.
    property var cursorTwips: [0, 0, 0, 0]
    property var selectionRects: []
    readonly property real cursorX: (cursorTwips[0] - (pageRect[0] || 0)) * twipsScale
    readonly property real cursorY: (cursorTwips[1] - (pageRect[1] || 0)) * twipsScale
    readonly property real cursorW: cursorTwips[2] * twipsScale
    readonly property real cursorH: cursorTwips[3] * twipsScale
    property bool cursorVisible: false

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
                root.renderInFlight = false
                root.renderQueued = false
                root.cursorVisible = false
                root.selectionRects = []
            }
        }
    }

    Component.onCompleted: {
        bridge.onEvent = handleEvent
        bridge.connectToEngine()
    }

    function handleEvent(ev) {
        if (ev.doc_id !== docId)
            return
        switch (ev.event) {
        case "tiles_changed": renderTimer.restart(); break  // lazy: re-render full page
        case "cursor_changed": updateCursor(ev); break
        case "selection_changed": updateSelection(ev); break
        case "cursor_visible": cursorVisible = ev.visible; break
        case "size_changed": break  // could store for future use
        }
    }

    function updateCursor(ev) {
        cursorTwips = [ev.x, ev.y, ev.width, ev.height]
        cursorOverlay.opacity = 1
        blink.restart()
    }

    function updateSelection(ev) {
        selectionRects = ev.rects || []
    }

    // Engine key names (VCL codes resolved engine-side).
    readonly property var specialKeys: ({
        [Qt.Key_Return]: "Return", [Qt.Key_Enter]: "Return", [Qt.Key_Backspace]: "Backspace",
        [Qt.Key_Delete]: "Delete", [Qt.Key_Tab]: "Tab", [Qt.Key_Escape]: "Escape",
        [Qt.Key_Left]: "Left", [Qt.Key_Right]: "Right", [Qt.Key_Up]: "Up", [Qt.Key_Down]: "Down",
        [Qt.Key_Home]: "Home", [Qt.Key_End]: "End",
        [Qt.Key_PageUp]: "PageUp", [Qt.Key_PageDown]: "PageDown"
    })

    // Returns false for keys we don't forward (modifiers alone, Ctrl/Alt shortcuts).
    function forwardKey(type, event) {
        if (docId < 0 || (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)))
            return false
        const cmd = { cmd: "key", doc_id: docId, type: type, char_code: 0, key_code: 0 }
        const name = specialKeys[event.key]
        if (name !== undefined)
            cmd.key = name
        else if (event.text.length > 0 && event.text.charCodeAt(0) >= 0x20)
            cmd.char_code = event.text.codePointAt(0)
        else
            return false
        bridge.send(cmd, function (r) {
            if (!r.ok)
                console.warn("key " + type + " failed: " + r.error)
        })
        return true
    }

    // Coalesces bursts of tiles_changed into one render, one at a time.
    Timer {
        id: renderTimer
        interval: 30
        onTriggered: root.requestFirstPage()
    }

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
        if (renderInFlight) {
            renderQueued = true
            return
        }
        renderInFlight = true
        bridge.send({
            cmd: "tile", doc_id: docId, part: 0,
            x: pageRect[0], y: pageRect[1], width: pageRect[2], height: pageRect[3],
            // 1440 twips/inch, 96 px/inch -> 15 twips per CSS pixel.
            px_width: Math.round(pageRect[2] / 15 * renderScale)
        }, function (r) {
            loading = false
            renderInFlight = false
            if (!r.ok) {
                errorText = "Render failed: " + r.error
                return
            }
            tileSource = "data:image/png;base64," + r.tile
            if (renderQueued) {
                renderQueued = false
                requestFirstPage()
            }
        })
    }

    Flickable {
        id: view
        anchors { fill: parent; bottomMargin: statusBar.height }
        contentWidth: Math.max(width, page.width + 64)
        contentHeight: page.height + 64
        clip: true

        focus: true
        Keys.onPressed: function (event) { event.accepted = root.forwardKey("input", event) }
        Keys.onReleased: function (event) { event.accepted = root.forwardKey("up", event) }

        Rectangle {
            anchors.fill: page
            anchors.margins: -1
            visible: root.pageShown
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
            retainWhileLoading: true
            cache: false
            smooth: true
            mipmap: true
            fillMode: Image.PreserveAspectFit
            width: Math.min(sourceSize.width, view.width - 64)
            height: sourceSize.width > 0 ? width * sourceSize.height / sourceSize.width : 0
            onStatusChanged: if (status === Image.Ready) root.pageShown = true
        }

        Repeater {
            model: root.selectionRects
            Rectangle {
                required property var modelData // [x, y, w, h] twips
                x: page.x + (modelData[0] - root.pageRect[0]) * root.twipsScale
                y: page.y + (modelData[1] - root.pageRect[1]) * root.twipsScale
                width: modelData[2] * root.twipsScale
                height: modelData[3] * root.twipsScale
                color: theme.accent
                opacity: 0.3
            }
        }

        Rectangle {
            id: cursorOverlay
            visible: root.cursorVisible && root.pageShown && root.selectionRects.length === 0
            x: page.x + root.cursorX
            y: page.y + root.cursorY
            width: Math.max(root.cursorW, 2)
            height: root.cursorH
            color: theme.accent

            Timer {
                id: blink
                interval: 500
                running: cursorOverlay.visible
                repeat: true
                onTriggered: cursorOverlay.opacity = cursorOverlay.opacity > 0 ? 0 : 1
            }
        }
    }

    // Centered message while there is nothing to show (or something broke).
    Text {
        visible: !root.pageShown || root.errorText !== ""
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
