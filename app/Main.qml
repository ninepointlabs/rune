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

    // Format painter: paintedStyle (from get_char_style) is applied to the
    // next click/selection; sticky mode (double-click) keeps painting until Esc.
    property bool paintMode: false
    property bool paintModeSticky: false
    property var paintedStyle: null
    // A document click/drag in paint mode is waiting for its selection to settle.
    property bool paintPending: false

    // Path of the document on screen; empty for a new, never-saved one.
    property string currentPath: documentPath
    readonly property string documentName: currentPath
        ? currentPath.substring(currentPath.lastIndexOf("/") + 1) : "Untitled"

    readonly property var fontNames: ["Liberation Sans", "Liberation Serif", "Liberation Mono",
                                      "DejaVu Sans", "DejaVu Serif", "Noto Sans"]
    readonly property var fontSizes: [8, 10, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48]
    // Text color presets: [label, engine hex]; "auto" is the default color.
    readonly property var textColors: [["Automatic", "auto"], ["Black", "#000000"], ["Red", "#cc0000"],
                                       ["Blue", "#0066cc"], ["Green", "#009933"]]
    readonly property real currentFontSize: parseFloat(formattingState.FontHeight)
    // Text color at the cursor as "#rrggbb", or "auto" (state reports -1).
    readonly property string currentColorHex: {
        const c = Number(formattingState.Color)
        return formattingState.Color === undefined || !(c >= 0) ? "auto"
            : "#" + c.toString(16).padStart(6, "0")
    }
    // Index into textColors of the color at the cursor; -1 for other colors.
    readonly property int currentColorIndex: textColors.findIndex(t => t[1] === currentColorHex)

    width: 1000
    height: 1100
    visible: true
    title: documentName ? documentName + " — Rune" : "Rune"
    color: theme.background

    // Objects the extracted UI components reach through appRoot.
    property alias bridge: bridge
    property alias pageTiles: pageTiles
    property alias paintTimer: paintTimer

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

    ClipboardHelper { id: clipboard }

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
        case "cursor_changed": updateCursor(ev); markDirty(ev.y, ev.height); fetchFormatState(); settlePaint(); break
        case "selection_changed": updateSelection(ev); fetchFormatState(); settlePaint(); break
        case "cursor_visible": cursorVisible = ev.visible; break
        case "size_changed": if (ev.page_rects) setPageRects(ev.page_rects, true); break
        case "autosaved": dirty = false; break
        }
    }

    function save() {
        if (docId < 0)
            return
        if (!currentPath) {
            flashStatus("Save As is not yet implemented; this document has no file yet")
            return
        }
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

    // Character/paragraph formatting via a .uno: command (".uno:Bold", ...),
    // with an optional value ("Liberation Serif" for .uno:CharFontName).
    function applyFormat(command, args) {
        if (docId < 0)
            return
        markEdited()
        const cmd = { cmd: "format", doc_id: docId, command: command }
        if (args !== undefined)
            cmd.args = String(args)
        bridge.send(cmd, function (r) {
            if (!r.ok)
                console.warn("format " + command + " failed: " + r.error)
        })
        fetchFormatState()
    }

    // Paragraph spacing step (directional only — see the toolbar comment
    // and engine/README.md for why exact point values aren't supported).
    function applyParaSpacing(direction) {
        if (docId < 0)
            return
        markEdited()
        bridge.send({ cmd: "para", doc_id: docId, direction: direction }, function (r) {
            if (!r.ok)
                console.warn("para " + direction + " failed: " + r.error)
        })
    }

    // Text color: "#RRGGBB" or "auto".
    function applyColor(hex) {
        if (docId < 0)
            return
        markEdited()
        bridge.send({ cmd: "color", doc_id: docId, hex: hex }, function (r) {
            if (!r.ok)
                console.warn("color " + hex + " failed: " + r.error)
        })
        fetchFormatState()
    }

    function cycleFont() {
        const i = fontNames.indexOf(formattingState.CharFontName)
        applyFormat(".uno:CharFontName", fontNames[(i + 1) % fontNames.length])
    }

    // Steps the font size to the next preset up (+1) or down (-1).
    function stepFontSize(direction) {
        const cur = isNaN(currentFontSize) ? 12 : currentFontSize
        const next = direction > 0 ? fontSizes.find(s => s > cur)
                                   : fontSizes.slice().reverse().find(s => s < cur)
        if (next !== undefined)
            applyFormat(".uno:FontHeight", next)
    }

    function cycleColor() {
        applyColor(textColors[(currentColorIndex + 1) % textColors.length][1])
    }

    // Opens a blank document in place of the current one.
    function newDocument() {
        if (!bridge.connected)
            return
        const previous = docId, wasDirty = dirty
        bridge.send({ cmd: "new_md", markdown: "" }, function (r) {
            if (!r.ok) {
                flashStatus("New document failed: " + r.error)
                return
            }
            // A dirty document stays open so its autosave recovery copy survives.
            if (previous >= 0 && !wasDirty)
                bridge.send({ cmd: "close", doc_id: previous }, function () {})
            currentPath = ""
            adoptDocument(r)
        })
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

    // Captures the formatting at the cursor and enters paint mode.
    function startPaintMode(sticky) {
        const doc = docId
        if (doc < 0)
            return
        paintPending = false
        paintModeSticky = sticky
        paintMode = true
        bridge.send({ cmd: "get_char_style", doc_id: doc }, function (r) {
            if (doc !== root.docId)
                return
            if (r.ok) {
                root.paintedStyle = r.style
            } else {
                console.warn("get_char_style failed: " + r.error)
                root.exitPaintMode()
            }
        })
    }

    function exitPaintMode() {
        paintMode = false
        paintModeSticky = false
        paintPending = false
    }

    // Selection/cursor events after a paint click: wait for them to settle
    // so the engine's cached state reflects the new selection.
    function settlePaint() {
        if (paintPending)
            paintTimer.restart()
    }

    Timer {
        id: paintTimer
        interval: 150
        onTriggered: {
            if (!root.paintPending || !root.paintMode || root.docId < 0 || !root.paintedStyle)
                return
            root.paintPending = false
            root.markEdited()
            bridge.send({ cmd: "apply_char_style", doc_id: root.docId, style: root.paintedStyle }, function (r) {
                if (!r.ok)
                    console.warn("apply_char_style failed: " + r.error)
            })
            root.fetchFormatState()
            if (!root.paintModeSticky)
                root.exitPaintMode()
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
        statusBar.flashAnim.restart()
    }

    function updateCursor(ev) {
        cursorTwips = [ev.x, ev.y, ev.width, ev.height]
        // LOK's CURSOR_VISIBLE callback is unreliable (observed: it never
        // fires after clicks or typing in this build), so treat any cursor
        // position update as "now visible" ourselves.
        cursorVisible = true
        view.cursorOverlay.opacity = 1
        view.blink.restart()
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

    // Inverse of twipsToView: content point -> {x, y} twips (integers, as
    // the engine requires), or null before the layout exists.
    function viewToTwips(vx, vy) {
        if (pageLayout.length === 0 || pageLayout.length !== pageRects.length || twipsScale <= 0)
            return null
        const i = pageIndexForViewY(vy)
        const box = pageLayout[i], p = pageRects[i]
        return {
            x: Math.round(p[0] + (vx - box.x) / twipsScale),
            y: Math.round(p[1] + (vy - box.y) / twipsScale)
        }
    }

    function sendMouse(type, vx, vy) {
        if (docId < 0)
            return
        const t = viewToTwips(vx, vy)
        if (!t)
            return
        bridge.send({ cmd: "mouse", doc_id: docId, type: type, x: t.x, y: t.y,
                      count: 1, buttons: 1, modifiers: 0 }, function (r) {
            if (!r.ok)
                console.warn("mouse " + type + " failed: " + r.error)
        })
    }

    // Engine key names (VCL codes resolved engine-side).
    readonly property var specialKeys: ({
        [Qt.Key_Return]: "Return", [Qt.Key_Enter]: "Return", [Qt.Key_Backspace]: "Backspace",
        [Qt.Key_Delete]: "Delete", [Qt.Key_Escape]: "Escape",
        [Qt.Key_Left]: "Left", [Qt.Key_Right]: "Right", [Qt.Key_Up]: "Up", [Qt.Key_Down]: "Down",
        [Qt.Key_Home]: "Home", [Qt.Key_End]: "End",
        [Qt.Key_PageUp]: "PageUp", [Qt.Key_PageDown]: "PageDown"
    })
    // Named keys that change the text (the rest only navigate).
    readonly property var editingKeys: ["Return", "Backspace", "Delete"]

    // Ctrl+key formatting shortcuts.
    readonly property var formatShortcuts: ({
        [Qt.Key_B]: ".uno:Bold", [Qt.Key_I]: ".uno:Italic", [Qt.Key_U]: ".uno:Underline",
        // Clear formatting: Ctrl+Space clears character formatting (fonts,
        // colors, sizes) back to the style; Ctrl+Q resets paragraph
        // formatting (margins, indents, spacing) to style defaults.
        [Qt.Key_Space]: ".uno:SetDefault", [Qt.Key_Q]: ".uno:ResetAttributes"
    })

    // Ctrl+C/X: the engine returns the selected text; we own the system clipboard.
    function copySelection(cut) {
        bridge.send({ cmd: cut ? "cut" : "copy", doc_id: docId }, function (r) {
            if (!r.ok) {
                console.warn((cut ? "cut" : "copy") + " failed: " + r.error)
                return
            }
            if (r.text) {
                clipboard.setText(r.text)
                if (cut)
                    markEdited()
            }
        })
    }

    function pasteClipboard() {
        const text = clipboard.text()
        if (!text)
            return
        markEdited()
        bridge.send({ cmd: "paste", doc_id: docId, mime_type: "text/plain", data: text }, function (r) {
            if (!r.ok)
                console.warn("paste failed: " + r.error)
        })
    }

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
        if (mods === Qt.ControlModifier
                && (event.key === Qt.Key_C || event.key === Qt.Key_X || event.key === Qt.Key_V)) {
            if (type === "input" && !event.isAutoRepeat) {
                if (event.key === Qt.Key_V)
                    pasteClipboard()
                else
                    copySelection(event.key === Qt.Key_X)
            }
            return true
        }
        // Tab/Shift+Tab always change list level (Writer's DecrementLevel nests
        // deeper, IncrementLevel un-nests), even outside a list: we can't tell
        // from here whether the cursor is in one, so Tab never inserts a literal
        // tab character. A future shortcut could insert one explicitly.
        // Qt reports Shift+Tab as Key_Backtab.
        if ((mods & ~Qt.ShiftModifier) === 0 && (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)) {
            if (type === "input") {
                const promote = event.key === Qt.Key_Backtab || mods === Qt.ShiftModifier
                applyFormat(promote ? ".uno:IncrementLevel" : ".uno:DecrementLevel")
            }
            return true
        }
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
            return false
        // Escape leaves format painter mode instead of reaching the document.
        if (event.key === Qt.Key_Escape && paintMode) {
            if (type === "input")
                exitPaintMode()
            return true
        }
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
            currentPath = documentPath
            adoptDocument(r)
        })
    }

    // Shows the document from an open/new_md reply, dropping the previous one's view state.
    function adoptDocument(r) {
        docId = r.doc_id
        errorText = ""
        renderQueue = []
        dirtyPages = {}
        cursorVisible = false
        selectionRects = []
        formattingState = {}
        dirty = false
        pageTiles.clear()
        view.contentY = 0
        // Autosave is always on; the engine writes recovery copies every 30 s.
        bridge.send({ cmd: "autosave", doc_id: docId, enabled: true }, function (a) {
            if (!a.ok)
                console.warn("autosave enable failed: " + a.error)
        })
        setPageRects(r.page_rects || [r.page_rect], false)
        fetchFormatState()
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

    FileBar {
        id: fileBar
        appRoot: root
        anchors { left: parent.left; right: parent.right; top: parent.top }
    }

    Toolbar {
        id: toolbar
        appRoot: root
        anchors { left: parent.left; right: parent.right; top: fileBar.bottom }
    }

    DocumentCanvas {
        id: view
        appRoot: root
        anchors { left: parent.left; right: parent.right; top: toolbar.bottom; bottom: statusBar.top }
    }

    StatusBar {
        id: statusBar
        appRoot: root
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
    }
}
