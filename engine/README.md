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

### `new_md`

```json
→ {"id": 6, "cmd": "new_md", "markdown": "# Title\n\nSome **bold** text.\n"}
← {"id": 6, "ok": true, "doc_id": 1, "parts": 1, "pages": 1, ...}
```

Creates a new, unsaved Writer document from Markdown and replies with the same
fields as `open`. `markdown` is required (may be empty for a blank document).
The engine converts it to HTML (`md_to_html.cpp`) and pastes that as
`text/html`. Supported: ATX headings, paragraphs, `**bold**`/`__bold__`,
`*italic*`/`_italic_`, flat `-`/`*`/`+` and `1.` lists, fenced code blocks,
`` `inline code` ``, `[links](url)` and `---` rules. Anything else stays as
literal text.

### `export_md`

```json
→ {"id": 7, "cmd": "export_md", "doc_id": 1}
← {"id": 7, "ok": true, "markdown": "Title\n\nSome bold text.\n"}
```

Works on any open document. It is exported as UTF-8 plain text to a temp
directory (removed afterwards); each non-empty line, trimmed, becomes one
paragraph, with a blank line between paragraphs. **Lossy:** headings,
emphasis, links and code formatting are dropped. List items keep LO's
text-export markers (`•`, `1.`).

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

### `mouse`

```json
→ {"id": 6, "cmd": "mouse", "doc_id": 0, "type": "down", "x": 1440, "y": 1440,
   "count": 1, "buttons": 1, "modifiers": 0}
← {"id": 6, "ok": true}
```

| field       | meaning |
|-------------|---------|
| `doc_id`    | Required. |
| `type`      | Required: `down`, `move` or `up`. |
| `x`, `y`    | Required integers, **in twips** (document coordinates). |
| `count`     | Optional click count (1–3), default 1. 2 = double-click (select word). |
| `buttons`   | Optional LOK button mask, default 1 (left). |
| `modifiers` | Optional LOK modifier mask, default 0. |

LOK handles the event asynchronously; the caret move arrives later as a
`cursor_changed` push event, a drag selection as `selection_changed`.

### `copy` / `cut`

```json
→ {"id": 7, "cmd": "copy", "doc_id": 0}
← {"id": 7, "ok": true, "text": "selected text"}
```

`text` is the current selection as UTF-8 plain text, `""` when nothing is
selected. `cut` returns the same and then deletes the selection (a Delete
key press); with no selection it does nothing. The engine does not touch
the system clipboard — the client does.

### Push events

Lines without an `id`, sent to every client. All carry `event` and `doc_id`.

| event               | fields |
|---------------------|--------|
| `tiles_changed`     | `x`, `y`, `width`, `height`: invalidated area in twips. |
| `cursor_changed`    | `x`, `y`, `width`, `height`: caret rect in twips. |
| `cursor_visible`    | `visible`. |
| `selection_changed` | `rects` (empty = cleared); `start`/`end` handle rects when non-empty. |
| `size_changed`      | `doc_size`, plus `pages` and `page_rects` re-read after the layout change. |

### `ai`

LLM provider integration (`ai_manager.cpp`). **Skeleton:** no HTTP calls are
made yet; `send` returns a placeholder. Dispatches on `action`:

```json
→ {"id": 8, "cmd": "ai", "action": "list_providers"}
← {"id": 8, "ok": true, "providers": ["chatgpt", "claude", "grok"]}

→ {"id": 9, "cmd": "ai", "action": "status"}
← {"id": 9, "ok": true, "providers": {"chatgpt": {"configured": false},
   "claude": {"configured": true}, "grok": {"configured": false}}}

→ {"id": 10, "cmd": "ai", "action": "set_token", "provider": "claude", "token": "sk-..."}
← {"id": 10, "ok": true}

→ {"id": 11, "cmd": "ai", "action": "send", "provider": "claude", "model": "claude-sonnet-4-6",
   "system": "You are a writing assistant.", "user": "Improve this paragraph: ..."}
← {"id": 11, "ok": true, "content": "[AI response will go here]", "model": "claude", "provider": "claude"}
```

| action           | fields |
|------------------|--------|
| `list_providers` | — Providers are `claude`, `chatgpt`, `grok` (sorted by name). |
| `status`         | — `configured` is true once a token has been set. |
| `set_token`      | `provider`, `token` (non-empty). Held in engine memory only, lost on exit; will move to the system keychain. |
| `send`           | `provider`, `model`, `user` required; `system` optional. Does not currently require a token. `model` in the reply is the provider name for now. |

Unknown providers, missing fields and unknown actions return `ok: false`.

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
