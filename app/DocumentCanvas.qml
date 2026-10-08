import QtQuick

// The document view plus the centered loading/error message over it. The
// root Item exposes the Flickable state Main.qml's functions read and write.
Item {
    id: canvasRoot
    required property var appRoot   // the Main.qml Window instance
    property alias contentY: view.contentY
    property alias contentWidth: view.contentWidth
    property alias contentHeight: view.contentHeight
    property alias cursorOverlay: cursorOverlay
    property alias blink: blink

    Flickable {
        id: view
        anchors.fill: parent
        contentWidth: Math.max(width, canvasRoot.appRoot.maxPageTwipsW * canvasRoot.appRoot.twipsScale + 2 * canvasRoot.appRoot.pageMargin)
        contentHeight: {
            const last = canvasRoot.appRoot.pageLayout[canvasRoot.appRoot.pageLayout.length - 1]
            return last ? last.y + last.h + canvasRoot.appRoot.pageMargin : 0
        }
        clip: true

        onContentYChanged: canvasRoot.appRoot.updateViewport()
        onHeightChanged: canvasRoot.appRoot.updateViewport()

        focus: true
        Keys.onPressed: function (event) {
            if (event.key === Qt.Key_S && (event.modifiers & Qt.ControlModifier)) {
                if (!event.isAutoRepeat)
                    canvasRoot.appRoot.save()
                event.accepted = true
                return
            }
            event.accepted = canvasRoot.appRoot.forwardKey("input", event)
        }
        Keys.onReleased: function (event) { event.accepted = canvasRoot.appRoot.forwardKey("up", event) }

        // The page column; boxes come from root.pageLayout so overlays and
        // viewport tracking share one source of truth for page positions.
        Repeater {
            model: canvasRoot.appRoot.pageTiles

            Item {
                id: pageItem
                required property int index
                required property string tile
                readonly property var box: canvasRoot.appRoot.pageLayout[index] || ({ x: 0, y: 0, w: 0, h: 0 })
                // Decode only pages near the viewport; far ones keep just the PNG data.
                readonly property bool near: index >= canvasRoot.appRoot.firstVisible - canvasRoot.appRoot.preloadPages
                                             && index <= canvasRoot.appRoot.lastVisible + canvasRoot.appRoot.preloadPages

                x: box.x
                y: box.y
                width: box.w
                height: box.h

                // Drop shadow.
                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 3
                    anchors.topMargin: 3
                    anchors.rightMargin: -3
                    anchors.bottomMargin: -3
                    color: "#000000"
                    opacity: 0.35
                }

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -1
                    color: "#ffffff"
                    border.color: canvasRoot.appRoot.theme.muted
                    border.width: 1
                }

                Image {
                    anchors.fill: parent
                    source: pageItem.near ? pageItem.tile : ""
                    asynchronous: true
                    retainWhileLoading: true
                    cache: false
                    smooth: true
                    mipmap: true
                    fillMode: Image.PreserveAspectFit
                    onStatusChanged: if (status === Image.Ready) canvasRoot.appRoot.pageShown = true
                }
            }
        }

        // Click places the cursor, left-drag selects (coordinates go to the
        // engine as twips). preventStealing keeps the Flickable from turning
        // a drag into a scroll mid-selection; wheel/touchpad scrolling is
        // unaffected since this MouseArea has no wheel handler.
        MouseArea {
            id: docMouse
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            preventStealing: true
            cursorShape: canvasRoot.appRoot.paintMode ? Qt.CrossCursor : Qt.IBeamCursor
            onPressed: function (mouse) {
                view.forceActiveFocus()
                canvasRoot.appRoot.sendMouse("down", mouse.x, mouse.y)
            }
            onPositionChanged: function (mouse) {
                if (pressed)
                    canvasRoot.appRoot.sendMouse("move", mouse.x, mouse.y)
            }
            onReleased: function (mouse) {
                canvasRoot.appRoot.sendMouse("up", mouse.x, mouse.y)
                if (canvasRoot.appRoot.paintMode) {
                    canvasRoot.appRoot.paintPending = true
                    canvasRoot.appRoot.paintTimer.restart()
                }
            }
        }

        Repeater {
            model: canvasRoot.appRoot.selectionRects
            Rectangle {
                required property var modelData // [x, y, w, h] twips
                readonly property var box: canvasRoot.appRoot.twipsToView(modelData)
                x: box.x
                y: box.y
                width: box.w
                height: box.h
                color: canvasRoot.appRoot.theme.accent
                opacity: 0.3
            }
        }

        Rectangle {
            id: cursorOverlay
            visible: canvasRoot.appRoot.cursorVisible && canvasRoot.appRoot.pageShown && canvasRoot.appRoot.selectionRects.length === 0
            x: canvasRoot.appRoot.cursorView.x
            y: canvasRoot.appRoot.cursorView.y
            width: Math.max(canvasRoot.appRoot.cursorView.w, 2)
            height: canvasRoot.appRoot.cursorView.h
            color: canvasRoot.appRoot.theme.accent

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
        visible: !canvasRoot.appRoot.pageShown || canvasRoot.appRoot.errorText !== ""
        anchors.centerIn: view
        width: parent.width * 0.8
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: 16
        color: canvasRoot.appRoot.errorText !== "" ? canvasRoot.appRoot.theme.red : canvasRoot.appRoot.theme.foreground
        text: canvasRoot.appRoot.errorText !== "" ? canvasRoot.appRoot.errorText
            : !canvasRoot.appRoot.bridge.connected ? (canvasRoot.appRoot.bridge.errorString || "Connecting to engine…") + "\nRetrying…"
            : canvasRoot.appRoot.loading ? "Rendering " + canvasRoot.appRoot.documentName + "…"
            : ""
    }
}
