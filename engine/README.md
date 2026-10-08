# rune-engine

Headless LibreOfficeKit daemon. Pure C++17 (no Qt): LOK via `dlopen` from
`LO_PROGRAM_DIR`, libpng for tile encoding, POSIX sockets for transport.

```sh
cmake -S . -B build -G Ninja && cmake --build build
./build/engine/rune-engine [--socket-path PATH]   # daemon
./build/app/rune [--socket-path PATH] [document]  # frontend (retries until the engine is up)
```

- Default socket: `$XDG_RUNTIME_DIR/rune-engine.sock`, else `/tmp/rune-engine.sock`.
  Created with mode `0600`. A stale socket from a crashed engine is removed;
  if another engine is live on the path, startup fails with exit code 1.
- Logs go to **stderr**. stdout is unused.
- `SIGINT`/`SIGTERM` shut down cleanly (socket is unlinked).
- Exit is via `_Exit()`: LO 26.8 segfaults in libswlo static destructors
  during a normal `exit()`.
- Single-threaded `poll()` loop; LOK is not thread-safe. Multiple clients may
  connect at once; commands are handled one at a time in arrival order.
  Documents belong to the engine, not to a connection — a client disconnecting
  does not close its documents.

## Protocol

JSON lines over a Unix stream socket: one JSON object per line, `\n`
terminated, in both directions. Every request may carry an `id` (number or
string); it is echoed verbatim in the reply. Each request gets exactly one
reply, in order.

Every reply has `ok`. On failure:

```json
{"id": 7, "ok": false, "error": "human-readable message"}
```

Malformed JSON gets `{"id": null, "ok": false, "error": "invalid JSON"}`.
Lines over 16 MiB drop the connection.

Coordinates are in **twips** (1/1440 inch; 15 twips = 1 CSS pixel at 96 DPI).

### `ping`

```json
→ {"id": 1, "cmd": "ping"}
← {"id": 1, "ok": true}
```

### `open`

```json
→ {"id": 2, "cmd": "open", "path": "/path/to/doc.docx"}
← {"id": 2, "ok": true, "doc_id": 0, "parts": 1, "pages": 1,
   "page_rect": [284, 284, 11906, 16838], "page_rects": [[284, 284, 11906, 16838]],
   "doc_size": [12474, 17406]}
```

| field       | meaning |
|-------------|---------|
| `doc_id`    | Handle for later commands. Monotonic, never reused within an engine run. |
| `parts`     | LOK parts (1 for Writer; sheets for Calc; slides for Impress). |
| `pages`     | Page count (Writer). 1 for document types without page rects. |
| `page_rect` | First page `[x, y, w, h]` in twips, in document coordinates (Writer pages sit inside a margin, so `x`/`y` are usually non-zero). Falls back to `[0, 0, doc_w, doc_h]`. |
| `page_rects`| Every page `[x, y, w, h]` in twips, top to bottom; `page_rects[0] == page_rect`. Always at least one entry. |
| `doc_size`  | Whole document `[w, h]` in twips. |

`path` may be relative to the engine's working directory; it is resolved with
`realpath()`. Supported: `.docx`, `.doc`, `.odt`, `.rtf`, `.txt`, and any
format LibreOffice can import. LOK picks the filter from the file extension.

### `tile`

```json
→ {"id": 3, "cmd": "tile", "doc_id": 0, "part": 0,
   "x": 284, "y": 284, "width": 11906, "height": 16838, "px_width": 1191}
← {"id": 3, "ok": true, "px_width": 1191, "px_height": 1684, "tile": "<base64 PNG>"}
```

| field                    | meaning |
|--------------------------|---------|
| `doc_id`                 | Required. |
| `part`                   | Optional, default 0. |
| `x`, `y`, `width`, `height` | Required. Area of the document to render, **in twips**. |
| `px_width`, `px_height`  | Optional output size in pixels. Neither → 96 DPI (`width/15 × height/15`). One → the other follows the area's aspect ratio. Max 8192 each. |

`tile` is a base64-encoded RGBA PNG (straight alpha). In QML it can be used
directly as `"data:image/png;base64," + tile`.

### Push events

Lines without an `id`, sent to every client. All carry `event` and `doc_id`.

| event               | fields |
|---------------------|--------|
| `tiles_changed`     | `x`, `y`, `width`, `height`: invalidated area in twips. |
| `cursor_changed`    | `x`, `y`, `width`, `height`: caret rect in twips. |
| `cursor_visible`    | `visible`. |
| `selection_changed` | `rects` (empty = cleared); `start`/`end` handle rects when non-empty. |
| `size_changed`      | `doc_size`, plus `pages` and `page_rects` re-read after the layout change. |

### `close`

```json
→ {"id": 4, "cmd": "close", "doc_id": 0}
← {"id": 4, "ok": true}
```

### `quit`

```json
→ {"id": 5, "cmd": "quit"}
← {"id": 5, "ok": true}
```

The reply is sent, then the engine unlinks its socket and exits 0.

## Testing by hand

```sh
./build/engine/rune-engine --socket-path /tmp/e.sock &
printf '%s\n' '{"id":1,"cmd":"ping"}' '{"id":2,"cmd":"open","path":"samples/test.docx"}' \
  '{"id":3,"cmd":"quit"}' | socat - UNIX-CONNECT:/tmp/e.sock
```
