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
    // Pages rendered beyond each edge of the viewport.
    readonly property int preloadPages: 3
    // Most page tiles kept in memory; the farthest from view are dropped.
    readonly property int maxBufferedPages: 50
    readonly property int pageGap: 16
    readonly property int pageMargin: 32

    property int docId: -1
    // Every page [x, y, w, h] in twips, document coordinates.
    property var pageRects: []
    readonly property int pageCount: pageRects.length
    property string errorText: ""
    property bool loading: false
    // Set once the first render lands; re-renders keep the old image up.
    property bool pageShown: false

    // Tile render queue: one request in flight, page indices waiting.
    property bool renderInFlight: false
    property int renderingPage: -1
    property bool renderingDirtied: false
    property var renderQueue: []
    // Pages invalidated since renderTimer last fired (index -> true).
    property var dirtyPages: ({})

    // Viewport, as page indices (updated on scroll/resize).
    property int firstVisible: 0
    property int lastVisible: 0
    property int currentPage: 0

    // Twips (document coords) -> view pixels. Every page shares one scale,
    // sized so the widest page fits the window.
    readonly property real maxPageTwipsW: pageRects.reduce((m, r) => Math.max(m, r[2]), 0)
    readonly property real twipsScale: maxPageTwipsW > 0
        ? Math.min(maxPageTwipsW / 15, view.width - 2 * pageMargin) / maxPageTwipsW : 0
    // Page boxes in Flickable content coordinates: [{x, y, w, h}, ...].
    readonly property var pageLayout: {
        const out = []
        let y = pageMargin
        for (const r of pageRects) {
            const w = r[2] * twipsScale, h = r[3] * twipsScale
            out.push({ x: (view.contentWidth - w) / 2, y: y, w: w, h: h })
            y += h + pageGap
        }
        return out
    }

    // Cursor and selection as last reported by the engine, in twips
    // [x, y, w, h]; the overlays convert so they follow window resizes.
    property var cursorTwips: [0, 0, 0, 0]
    property var selectionRects: []
    readonly property var cursorView: twipsToView(cursorTwips)
    property bool cursorVisible: false
    // Keyboard-driven cursor moves scroll the cursor into view.
    property double lastKeyTime: 0

    // Edits not yet written by a save or autosave; editSeq counts edits so a
    // save reply doesn't clear edits typed while it was in flight.
    property bool dirty: false
    property int editSeq: 0
    // Brief status-bar message ("Saved"); empty shows the document name.
    property string statusFlash: ""

    // Formatting at the cursor from get_state: {Bold: true, StyleApply: "Heading 1", ...}.
    property var formattingState: ({})

    readonly property string documentName: documentPath.substring(documentPath.lastIndexOf("/") + 1)

    width: 1000
    height: 1100
    visible: true
    title: documentName ? documentName + " — Rune" : "Rune"
    color: theme.background

    onPageLayoutChanged: updateViewport()

    // One row per page: the latest tile (data URL) and whether it is stale.
    ListModel { id: pageTiles }

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
                root.renderingPage = -1
                root.renderQueue = []
                root.dirtyPages = {}
                root.cursorVisible = false
                root.selectionRects = []
                root.formattingState = {}
                // The reopened document is whatever is on disk.
                root.dirty = false
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
        case "tiles_changed": markDirty(ev.y, ev.height); break
        // Typing moves the cursor; re-render the page it is on.
        case "cursor_changed": updateCursor(ev); markDirty(ev.y, ev.height); fetchFormatState(); break
        case "selection_changed": updateSelection(ev); fetchFormatState(); break
        case "cursor_visible": cursorVisible = ev.visible; break
        case "size_changed": if (ev.page_rects) setPageRects(ev.page_rects, true); break
        case "autosaved": dirty = false; break
        }
    }

    function save() {
        if (docId < 0)
            return
        const seq = editSeq
        bridge.send({ cmd: "save", doc_id: docId }, function (r) {
            if (!r.ok) {
                console.warn("save failed: " + r.error)
                flashStatus("Save failed: " + r.error)
                return
            }
            if (seq === editSeq)
                dirty = false
            flashStatus("Saved")
        })
    }

    // Character/paragraph formatting via a .uno: command (".uno:Bold", ...).
    function applyFormat(command) {
        if (docId < 0)
            return
        markEdited()
        bridge.send({ cmd: "format", doc_id: docId, command: command }, function (r) {
            if (!r.ok)
                console.warn("format " + command + " failed: " + r.error)
        })
        fetchFormatState()
    }

    // Paragraph style by engine name ("Normal", "Heading 1", ...).
    function applyStyle(name) {
        if (docId < 0)
            return
        markEdited()
        bridge.send({ cmd: "style", doc_id: docId, name: name }, function (r) {
            if (!r.ok)
                console.warn("style " + name + " failed: " + r.error)
        })
        fetchFormatState()
    }

    function markEdited() {
        dirty = true
        ++editSeq
    }

    // Queries the formatting state once cursor moves and edits settle.
    function fetchFormatState() {
        if (docId >= 0)
            formatStateTimer.restart()
    }

    Timer {
        id: formatStateTimer
        interval: 150
        onTriggered: {
            const doc = root.docId
            if (doc < 0)
                return
            bridge.send({ cmd: "get_state", doc_id: doc }, function (r) {
                if (doc !== root.docId)
                    return
                if (r.ok)
                    root.formattingState = r.state
                else
                    console.warn("get_state failed: " + r.error)
            })
        }
    }

    // Engine `style` names -> StyleApply values reported for them.
    readonly property var styleStateNames: ({
        "Normal": ["Default Paragraph Style", "Standard"],
        "Heading 1": ["Heading 1"], "Heading 2": ["Heading 2"], "Heading 3": ["Heading 3"]
    })

    function styleActive(name) {
        return styleStateNames[name].indexOf(formattingState.StyleApply) >= 0
    }

    function flashStatus(message) {
        statusFlash = message
        flashAnim.restart()
    }

    function updateCursor(ev) {
        cursorTwips = [ev.x, ev.y, ev.width, ev.height]
        cursorOverlay.opacity = 1
        blink.restart()
        if (Date.now() - lastKeyTime < 1000)
            ensureCursorVisible()
    }

    function updateSelection(ev) {
        selectionRects = ev.rects || []
    }

    function ensureCursorVisible() {
        const c = cursorView
        const pad = 24
        if (c.y < view.contentY + pad)
            view.contentY = Math.max(0, c.y - pad)
        else if (c.y + c.h > view.contentY + view.height - pad)
            view.contentY = Math.max(0, Math.min(view.contentHeight - view.height,
                                                 c.y + c.h - view.height + pad))
    }

    // Index of the page holding twips y; gaps belong to the page above.
    function pageIndexForTwipsY(ty) {
        let i = 0
        while (i + 1 < pageRects.length && pageRects[i + 1][1] <= ty)
            ++i
        return i
    }

    // Index of the page at content y (view pixels); gaps belong to the page above.
    function pageIndexForViewY(vy) {
        let i = 0
        while (i + 1 < pageLayout.length && pageLayout[i + 1].y <= vy)
            ++i
        return i
    }

    // [x, y, w, h] twips -> {x, y, w, h} in Flickable content coordinates.
    function twipsToView(r) {
        if (pageRects.length === 0 || pageLayout.length !== pageRects.length)
            return { x: 0, y: 0, w: 0, h: 0 }
        const i = pageIndexForTwipsY(r[1])
        const p = pageRects[i], box = pageLayout[i]
        return {
            x: box.x + (r[0] - p[0]) * twipsScale,
            y: box.y + (r[1] - p[1]) * twipsScale,
            w: r[2] * twipsScale,
            h: r[3] * twipsScale
        }
    }

    // Engine key names (VCL codes resolved engine-side).
    readonly property var specialKeys: ({
        [Qt.Key_Return]: "Return", [Qt.Key_Enter]: "Return", [Qt.Key_Backspace]: "Backspace",
        [Qt.Key_Delete]: "Delete", [Qt.Key_Tab]: "Tab", [Qt.Key_Escape]: "Escape",
        [Qt.Key_Left]: "Left", [Qt.Key_Right]: "Right", [Qt.Key_Up]: "Up", [Qt.Key_Down]: "Down",
        [Qt.Key_Home]: "Home", [Qt.Key_End]: "End",
        [Qt.Key_PageUp]: "PageUp", [Qt.Key_PageDown]: "PageDown"
    })
    // Named keys that change the text (the rest only navigate).
    readonly property var editingKeys: ["Return", "Backspace", "Delete", "Tab"]

    // Ctrl+key formatting shortcuts.
    readonly property var formatShortcuts: ({
        [Qt.Key_B]: ".uno:Bold", [Qt.Key_I]: ".uno:Italic", [Qt.Key_U]: ".uno:Underline"
    })

    // Returns false for keys we don't forward (modifiers alone, other Ctrl/Alt shortcuts).
    function forwardKey(type, event) {
        if (docId < 0)
            return false
        const mods = event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier | Qt.ShiftModifier)
        if (mods === Qt.ControlModifier && formatShortcuts[event.key] !== undefined) {
            if (type === "input" && !event.isAutoRepeat)
                applyFormat(formatShortcuts[event.key])
            return true
        }
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
            return false
        const cmd = { cmd: "key", doc_id: docId, type: type, char_code: 0, key_code: 0 }
        const name = specialKeys[event.key]
        if (name !== undefined)
            cmd.key = name
        else if (event.text.length > 0 && event.text.charCodeAt(0) >= 0x20)
            cmd.char_code = event.text.codePointAt(0)
        else
            return false
        if (type === "input") {
            lastKeyTime = Date.now()
            if (cmd.char_code !== 0 || editingKeys.indexOf(cmd.key) >= 0)
                markEdited()
        }
        bridge.send(cmd, function (r) {
            if (!r.ok)
                console.warn("key " + type + " failed: " + r.error)
        })
        return true
    }

    // Coalesces bursts of invalidations into one round of page renders.
    Timer {
        id: renderTimer
        interval: 30
        onTriggered: {
            for (const key in root.dirtyPages) {
                const i = Number(key)
                if (i === root.renderingPage)
                    root.renderingDirtied = true
                else if (i < pageTiles.count && pageTiles.get(i).tile !== "")
                    pageTiles.setProperty(i, "stale", true)
            }
            root.dirtyPages = {}
            root.requestVisibleTiles()
        }
    }

    // Marks pages overlapping the twips band [y, y + h) for re-render.
    function markDirty(y, h) {
        for (let i = 0; i < pageRects.length; ++i) {
            const p = pageRects[i]
            if (y < p[1] + p[3] && y + h > p[1])
                dirtyPages[i] = true
        }
        renderTimer.restart()
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
            view.contentY = 0
            // Autosave is always on; the engine writes recovery copies every 30 s.
            bridge.send({ cmd: "autosave", doc_id: docId, enabled: true }, function (a) {
                if (!a.ok)
                    console.warn("autosave enable failed: " + a.error)
            })
            setPageRects(r.page_rects || [r.page_rect], false)
            fetchFormatState()
        })
    }

    // Adopts a new page list; with invalidate, every kept tile is marked stale.
    function setPageRects(rects, invalidate) {
        while (pageTiles.count > rects.length)
            pageTiles.remove(pageTiles.count - 1)
        for (let i = 0; i < pageTiles.count; ++i) {
            if (invalidate && pageTiles.get(i).tile !== "")
                pageTiles.setProperty(i, "stale", true)
        }
        while (pageTiles.count < rects.length)
            pageTiles.append({ tile: "", stale: false })
        pageRects = rects
        updateViewport()
    }

    // Recomputes which pages are on screen and loads tiles around them.
    function updateViewport() {
        if (pageLayout.length === 0)
            return
        firstVisible = pageIndexForViewY(view.contentY)
        lastVisible = pageIndexForViewY(view.contentY + view.height)
        currentPage = pageIndexForViewY(view.contentY + view.height / 2)
        requestVisibleTiles()
    }

    function requestVisibleTiles() {
        requestPageTiles(firstVisible - preloadPages, lastVisible + preloadPages)
    }

    // Queues tiles for pages [startPage, endPage] that are missing or stale.
    function requestPageTiles(startPage, endPage) {
        if (docId < 0)
            return
        const start = Math.max(0, startPage), end = Math.min(pageTiles.count - 1, endPage)
        for (let i = start; i <= end; ++i) {
            const row = pageTiles.get(i)
            if ((row.tile === "" || row.stale) && i !== renderingPage && renderQueue.indexOf(i) < 0)
                renderQueue.push(i)
        }
        pumpRender()
    }

    // Sends the next wanted tile request, nearest the current page first.
    // Pages scrolled out of range since being queued are skipped.
    function pumpRender() {
        if (renderInFlight)
            return
        const lo = firstVisible - preloadPages, hi = lastVisible + preloadPages
        renderQueue = renderQueue.filter(i => i >= lo && i <= hi && i < pageTiles.count)
        if (renderQueue.length === 0)
            return
        renderQueue.sort((a, b) => Math.abs(a - currentPage) - Math.abs(b - currentPage))
        const index = renderQueue.shift()
        const p = pageRects[index]
        const doc = docId
        renderInFlight = true
        renderingPage = index
        renderingDirtied = false
        bridge.send({
            cmd: "tile", doc_id: doc, part: 0,
            x: p[0], y: p[1], width: p[2], height: p[3],
            // 1440 twips/inch, 96 px/inch -> 15 twips per CSS pixel.
            px_width: Math.round(p[2] / 15 * renderScale)
        }, function (r) {
            renderInFlight = false
            renderingPage = -1
            if (doc !== docId)
                return // engine restarted or document reopened
            loading = false
            if (!r.ok) {
                errorText = "Render failed: " + r.error
                return
            }
            if (index < pageTiles.count) {
                pageTiles.setProperty(index, "tile", "data:image/png;base64," + r.tile)
                pageTiles.setProperty(index, "stale", renderingDirtied)
                if (renderingDirtied)
                    renderQueue.push(index)
                evictTiles()
            }
            pumpRender()
        })
    }

    // Keeps at most maxBufferedPages tiles, dropping those farthest from view.
    function evictTiles() {
        const loaded = []
        for (let i = 0; i < pageTiles.count; ++i) {
            if (pageTiles.get(i).tile !== "")
                loaded.push(i)
        }
        if (loaded.length <= maxBufferedPages)
            return
        loaded.sort((a, b) => Math.abs(b - currentPage) - Math.abs(a - currentPage))
        for (let k = 0; k < loaded.length - maxBufferedPages; ++k)
            pageTiles.set(loaded[k], { tile: "", stale: false })
    }

    // A toolbar button: shows `label`, highlighted while `active`.
    component ToolButton: Rectangle {
        id: button
        property string label
        property bool active: false
        property bool bold: false
        property bool italic: false
        property bool underline: false
        signal clicked()

        width: Math.max(28, buttonText.implicitWidth + 14)
        height: 28
        radius: 4
        color: active ? theme.accent : mouse.containsMouse ? Qt.alpha(theme.foreground, 0.1) : "transparent"
        opacity: root.docId >= 0 ? 1 : 0.5

        Text {
            id: buttonText
            anchors.centerIn: parent
            text: button.label
            color: button.active ? theme.background : theme.foreground
            font.pixelSize: 13
            font.bold: button.bold
            font.italic: button.italic
            font.underline: button.underline
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            enabled: root.docId >= 0
            onClicked: button.clicked()
        }
    }

    component ToolSeparator: Rectangle {
        width: 1
        height: 20
        anchors.verticalCenter: parent.verticalCenter
        color: theme.muted
    }

    Rectangle {
        id: toolbar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 40
        color: theme.lighter_background !== undefined ? theme.lighter_background : theme.background

        Row {
            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 4 }
            spacing: 4

            ToolButton {
                label: "B"; bold: true
                active: root.formattingState.Bold === true
                onClicked: root.applyFormat(".uno:Bold")
            }
            ToolButton {
                label: "I"; italic: true
                active: root.formattingState.Italic === true
                onClicked: root.applyFormat(".uno:Italic")
            }
            ToolButton {
                label: "U"; underline: true
                active: root.formattingState.Underline === true
                onClicked: root.applyFormat(".uno:Underline")
            }

            ToolSeparator {}

            Repeater {
                model: [["Normal", "Normal"], ["H1", "Heading 1"], ["H2", "Heading 2"], ["H3", "Heading 3"]]
                ToolButton {
                    required property var modelData
                    label: modelData[0]
                    active: root.styleActive(modelData[1])
                    onClicked: root.applyStyle(modelData[1])
                }
            }

            ToolSeparator {}

            ToolButton {
                label: "•"
                active: root.formattingState.DefaultBullet === true
                onClicked: root.applyFormat(".uno:DefaultBullet")
            }
            ToolButton {
                label: "1."
                active: root.formattingState.DefaultNumbering === true
                onClicked: root.applyFormat(".uno:DefaultNumbering")
            }

            ToolSeparator {}

            Repeater {
                model: [["L", "LeftPara"], ["C", "CenterPara"], ["R", "RightPara"]]
                ToolButton {
                    required property var modelData
                    label: modelData[0]
                    active: root.formattingState[modelData[1]] === true
                    onClicked: root.applyFormat(".uno:" + modelData[1])
                }
            }
        }

        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1
            color: theme.muted
        }
    }

    Flickable {
        id: view
        anchors { left: parent.left; right: parent.right; top: toolbar.bottom; bottom: statusBar.top }
        contentWidth: Math.max(width, root.maxPageTwipsW * root.twipsScale + 2 * root.pageMargin)
        contentHeight: {
            const last = root.pageLayout[root.pageLayout.length - 1]
            return last ? last.y + last.h + root.pageMargin : 0
        }
        clip: true

        onContentYChanged: root.updateViewport()
        onHeightChanged: root.updateViewport()

        focus: true
        Keys.onPressed: function (event) {
            if (event.key === Qt.Key_S && (event.modifiers & Qt.ControlModifier)) {
                if (!event.isAutoRepeat)
                    root.save()
                event.accepted = true
                return
            }
            event.accepted = root.forwardKey("input", event)
        }
        Keys.onReleased: function (event) { event.accepted = root.forwardKey("up", event) }

        // The page column; boxes come from root.pageLayout so overlays and
        // viewport tracking share one source of truth for page positions.
        Repeater {
            model: pageTiles

            Item {
                id: pageItem
                required property int index
                required property string tile
                readonly property var box: root.pageLayout[index] || ({ x: 0, y: 0, w: 0, h: 0 })
                // Decode only pages near the viewport; far ones keep just the PNG data.
                readonly property bool near: index >= root.firstVisible - root.preloadPages
                                             && index <= root.lastVisible + root.preloadPages

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
                    border.color: theme.muted
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
                    onStatusChanged: if (status === Image.Ready) root.pageShown = true
                }
            }
        }

        Repeater {
            model: root.selectionRects
            Rectangle {
                required property var modelData // [x, y, w, h] twips
                readonly property var box: root.twipsToView(modelData)
                x: box.x
                y: box.y
                width: box.w
                height: box.h
                color: theme.accent
                opacity: 0.3
            }
        }

        Rectangle {
            id: cursorOverlay
            visible: root.cursorVisible && root.pageShown && root.selectionRects.length === 0
            x: root.cursorView.x
            y: root.cursorView.y
            width: Math.max(root.cursorView.w, 2)
            height: root.cursorView.h
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
            id: statusText
            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 10
                      right: connection.left; rightMargin: 16 }
            color: theme.foreground
            font.family: "monospace"
            font.pixelSize: 12
            elide: Text.ElideRight
            text: root.statusFlash !== "" ? root.statusFlash
                  : root.documentName + (root.dirty ? " ●" : "")
                    + (root.docId >= 0 && root.pageCount > 0
                       ? "  ·  Page " + (root.currentPage + 1) + " of " + root.pageCount : "")

            // Shows the flash for 2 s, then fades back to the document name.
            SequentialAnimation {
                id: flashAnim
                PropertyAction { target: statusText; property: "opacity"; value: 1 }
                PauseAnimation { duration: 2000 }
                NumberAnimation { target: statusText; property: "opacity"; to: 0; duration: 200 }
                ScriptAction { script: root.statusFlash = "" }
                NumberAnimation { target: statusText; property: "opacity"; to: 1; duration: 200 }
            }
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
