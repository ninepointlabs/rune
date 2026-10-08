# Rune — Roadmap

Status as of 2026-10-08. See `engine/README.md` for the full protocol reference and documented, tested limitations of headless LibreOfficeKit.

## Done

- Document I/O: open/save `.docx`, `.doc`, `.odt`, `.txt`, `.md`; markdown import/export; autosave
- Multi-page scrolling with viewport-aware tile loading
- Full text editing: keyboard, mouse (click-to-cursor, drag-select), copy/cut/paste
- Formatting: Bold/Italic/Underline, paragraph styles (Normal, H1-H3), bullet/numbered lists with promote/demote, alignment, font/size, text color, clear formatting
- Format Painter
- Tables: insert, AutoFit, insert/delete row, insert/delete column
- Paragraph spacing (directional increase/decrease — exact point values not achievable headlessly, see `engine/README.md`)
- Layered toolbar UX: slim persistent bar + contextual strip (replaces a Word-style ribbon)
- AI provider skeleton (Claude/ChatGPT/Grok registry, token storage) — **no real HTTP calls yet**

## Not started — Microsoft Word top-10 feature audit

| Feature | Notes |
|---|---|
| **Section breaks** | Mixed page orientation, independent headers/margins per section, restart page numbering |
| **Headers & footers** | Including "different first page," "different odd/even," "link to previous" |
| **Tab stops & leader lines** | Needs a ruler widget — most UI-heavy item on this list |
| **Text wrap on images** | No image insertion at all yet — this blocks wrap modes too |
| **Table of Contents / outline navigation** | Builds on existing paragraph styles; needs `.uno:InsertMultiIndex` or similar, untested |
| **Repeat header rows (tables)** | Tried 3 UNO command names, none worked headlessly — needs the full UNO `XPropertySet` API, a heavier integration than `LibreOfficeKit.h` |
| **Exact paragraph spacing / keep-together / widow-orphan** | Same limitation as above — `.uno:ParagraphDialog` args don't apply headlessly (verified via tile-diff) |

## AI integration (the actual differentiator)

- [ ] Real OAuth flows for Claude, ChatGPT, Grok (skeleton only stores a raw token today)
- [ ] Streaming HTTP responses into the document or a sidebar
- [ ] Three interaction modes per the original architecture plan: inline edit, sidebar chat, draft generation
- [ ] Right-side dock UI (deferred from the toolbar redesign — reuse the same dock mechanism for AI chat + a future "inspector" panel)

## Housekeeping / known rough edges

- [ ] File → Open has no file picker yet (stub message only)
- [ ] Save As has no path picker (only plain Save to the already-open path)
- [ ] Command palette (Ctrl+K) — planned as part of the toolbar redesign but not yet built; this is where rare actions (insert table with a custom size, PDF export, etc.) should live instead of permanent toolbar space
- [ ] No undo/redo UI (the engine likely supports `.uno:Undo`/`.uno:Redo` already via `format`, needs toolbar/shortcut wiring and testing)
- [ ] No find/replace
- [ ] No spell check
- [ ] `.doc`/`.odt` round-trip fidelity not stress-tested on complex real-world documents, only the generated sample

## Architecture notes for contributors

- Engine (`engine/`) is a headless LibreOfficeKit daemon behind a JSON-lines Unix socket. Split into feature-scoped files: `commands_document.cpp`, `commands_editing.cpp`, `commands_formatting.cpp`, `commands_tables.cpp`. See `engine/engine_internal.h` for the shared `Engine` class declaration.
- Frontend (`app/`) is Qt Quick/QML, also feature-scoped: `Toolbar.qml`, `ContextualBar.qml`, `FileBar.qml`, `StatusBar.qml`, `DocumentCanvas.qml` composed by `Main.qml`.
- `engine/smoke_test.py` is the test suite (156 checks) — run it after any engine change. It starts a private engine instance, drives the full JSON protocol, and diffs rendered tiles to confirm visual changes actually happened (not just that a command returned `ok`).
- Before adding a UNO command that isn't already used elsewhere, verify it actually works headlessly by diffing a rendered tile before/after — `postUnoCommand()` never reports failure even for commands LOK silently ignores. Several "should work" commands (exact paragraph spacing, repeat header rows) turned out to be dead ends this way.