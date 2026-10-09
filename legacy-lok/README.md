# legacy-lok — retired implementation

This is Rune's **original** implementation: a two-process design (`engine/` — a headless LibreOfficeKit daemon behind a JSON-lines Unix socket — and `app/` — a Qt Quick frontend talking to it). It is kept here for reference and is **not** the current app.

It was replaced by [`../native/`](../native/): a single-process app built directly on Qt's `QTextDocument`/`QTextLayout`, with LibreOffice demoted to a one-shot batch conversion utility for `.docx` only. See the root [README.md](../README.md) and [TODO.md](../TODO.md) for why, and the full decision trail (including the real bugs found in both architectures, with evidence) in the project's Obsidian notes.

## Building it anyway

It still builds and its own test suite still passes:

```sh
cmake -S legacy-lok -B legacy-lok/build -G Ninja
cmake --build legacy-lok/build
python3 legacy-lok/engine/smoke_test.py
./legacy-lok/build/engine/rune-engine --socket-path /tmp/rune-legacy.sock &
./legacy-lok/build/app/rune --socket-path /tmp/rune-legacy.sock ../samples/test.docx
```

156 engine-level checks, last verified passing at cutover (2026-10-09).

## Why it was replaced

In short: LibreOfficeKit's tiled bitmap rendering has a real performance ceiling (~150ms per full-page re-render on a page with a table), and the two-process architecture (socket, JSON protocol, a whole command-dispatch layer) existed specifically to isolate LOK's thread-safety and shutdown issues — none of which apply to `QTextDocument`. The product direction also shifted toward being ODF-first rather than built around Word-format compatibility. Full rationale in the root README's architecture section.