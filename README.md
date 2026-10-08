# Rune

**An AI-native word processor for Linux, built for [Omarchy](https://github.com/basecamp/omarchy).**

Rune opens and edits `.docx`, `.doc`, `.odt`, `.txt`, and `.md` with real fidelity — backed by [LibreOfficeKit](https://wiki.documentfoundation.org/Development/LibreOfficeKit) as a headless document engine, not a from-scratch parser. The differentiator: every major LLM provider (Claude, ChatGPT, Grok) is a first-class citizen inside the editing environment, not a bolt-on plugin — invoke any model for inline edits, drafts, or idea generation without leaving the page.

Native Qt Quick/QML frontend. No Electron, no browser engine in the critical path.

## Why

Word processors on Linux are either heavyweight ports (LibreOffice Writer, OnlyOffice) or minimal text editors pretending to be word processors. None of them treat an LLM as a core editing primitive — AI is always a sidebar plugin bolted onto a 20-year-old codebase. Rune starts from the opposite assumption: the model is part of the writing surface, and the document engine should get out of the way.

## Status

Early and moving fast. Core editing — open, type, format, save, tables — works end to end and is covered by 156 passing engine-level tests. The AI integration (the actual point of this project) is still a skeleton. See [TODO.md](TODO.md) for the honest, current state of every feature, including the ones we tried and couldn't make work headlessly.

| Area | State |
|---|---|
| Document I/O (`.docx`, `.doc`, `.odt`, `.txt`, `.md`) | ✅ Working |
| Text editing (keyboard, mouse, clipboard) | ✅ Working |
| Formatting (styles, B/I/U, lists, alignment, color, font) | ✅ Working |
| Tables (insert, AutoFit, row/column ops) | ✅ Working |
| Multi-page rendering | ✅ Working |
| Autosave | ✅ Working |
| AI integration | 🚧 Provider registry only — no real HTTP calls yet |
| Headers/footers, section breaks, images, tab stops | ❌ Not started |

## Architecture

Two processes talking JSON over a Unix socket:

```
┌─────────────────────────┐        ┌──────────────────────────────┐
│   rune (Qt Quick/QML)    │◄──────►│   rune-engine (headless)      │
│                          │  JSON  │                                │
│  Toolbar · ContextualBar │  lines │  LibreOfficeKit (dlopen'd)     │
│  DocumentCanvas          │  over  │  JSON-line command dispatch   │
│  FileBar · StatusBar     │  a     │  commands_document.cpp        │
│                          │  Unix  │  commands_editing.cpp         │
│                          │ socket │  commands_formatting.cpp      │
│                          │        │  commands_tables.cpp          │
└─────────────────────────┘        └──────────────────────────────┘
```

**Why two processes.** LibreOfficeKit is not thread-safe and has a documented, awkward shutdown path in this LO version (a clean `exit()` can segfault in static destructors — worked around with `_Exit()`). Keeping it in its own process means a LOK crash doesn't take the UI down with it, and means the UI can reconnect and resume.

**Why LibreOfficeKit instead of a custom parser.** Format fidelity for `.docx` is a multi-year problem that LibreOffice has already solved. Building on it means Rune spends its engineering budget on the editing experience and the AI integration instead of re-deriving OOXML.

Full details: [engine/README.md](engine/README.md) (protocol reference, every command, and the limitations we found and documented by testing — not guessing) and [docs/architecture.md](docs/architecture.md) (original design plan).

## Building

Requires Arch/Omarchy with `libreoffice-fresh`, `libreoffice-fresh-sdk`, and Qt 6 (`qt6-base`, `qt6-declarative`):

```sh
sudo pacman -S libreoffice-fresh libreoffice-fresh-sdk qt6-base qt6-declarative

git clone https://github.com/ninepointlabs/rune.git
cd rune
cmake -S . -B build -G Ninja
cmake --build build
```

## Running

```sh
./run.sh samples/test.docx
```

Or manually, since it's two processes:

```sh
./build/engine/rune-engine --socket-path /tmp/rune.sock &
./build/app/rune --socket-path /tmp/rune.sock path/to/document.docx
```

## Testing

```sh
python3 engine/smoke_test.py
```

156 checks that start a private engine instance, drive the full JSON protocol (open, type, format, table ops, save, autosave), and diff rendered tiles to confirm changes actually took visual effect — not just that a command returned `ok`. LibreOfficeKit's `postUnoCommand` never reports failure even for commands it silently ignores, so "the engine said ok" and "the document actually changed" are different claims; the test suite checks both.

## Contributing

Read [engine/README.md](engine/README.md) before adding a new `.uno:` command — several commands that *should* work headlessly (exact paragraph spacing, repeat-header-rows) turned out to be dead ends once tested by diffing a rendered tile before and after. The pattern that works: test against a running engine instance directly before writing the feature, don't assume the UNO API shape.

See [TODO.md](TODO.md) for what's not built yet.

## License

MIT — see [LICENSE](LICENSE).