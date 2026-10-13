# Rune

**An AI-native, ODF-first word processor for Linux, built for [Omarchy](https://github.com/basecamp/omarchy).**

Rune opens and edits `.odt` (native), `.docx`/`.doc` (via a one-shot conversion bridge), and plans real OAuth-connected LLM integration (Claude, ChatGPT, Grok) as a first-class editing primitive — not a sidebar plugin bolted onto a 20-year-old codebase.

Single-process native Qt Quick/QML app, built directly on Qt's `QTextDocument`. No Electron, no browser engine, no second process, no IPC.

## Why

Word processors on Linux are either heavyweight ports (LibreOffice Writer, OnlyOffice) or minimal text editors pretending to be word processors. None of them treat an LLM as a core editing primitive — AI is always a sidebar plugin. Rune starts from the opposite assumption: the model is part of the writing surface. It's also deliberately **ODF-first** rather than built around Word-format compatibility — `.docx` is supported (real, tested, round-trips correctly) but it's a bridge format at the boundary, not the native model.

## Status

Early and moving fast. Core editing — open, type, format, save, tables, lists, pages with headers and footers, real-world `.docx`/`.odt` round-trips — works end to end and is covered by 399 passing, independently-verified checks. Sign in with your ChatGPT plan or with OpenRouter (Claude, GPT, Grok), then have the AI write into or rewrite your document directly, or ask it questions about it. See [TODO.md](TODO.md) for the complete, honest state of every feature, including the two things we tried and couldn't make work and documented with evidence rather than hiding.

| Area | State |
|---|---|
| Document I/O (`.odt` native, `.docx`/`.doc` via bridge) | ✅ Working |
| Text editing (keyboard, mouse, clipboard) | ✅ Working |
| Formatting (styles, B/I/U, lists w/ nesting, alignment, color, font) | ✅ Working |
| Tables (insert, row/column ops) | ✅ Working |
| Pages: paper size by locale, margins, orientation (File → Page setup) | ✅ Working |
| Headers and footers, with page number and page count fields | ✅ Working |
| Command palette (Ctrl+K) | ✅ Working |
| Named/inherited paragraph styles on import (`styles.xml`) | ✅ Working |
| Async `.docx` conversion (doesn't block the UI) | ✅ Working |
| AI sign-in (ChatGPT plan, OpenRouter) + streaming chat panel | ✅ Working |
| AI editing in the document (corrections in place, rewrite selection, write at cursor; one undo step) | ✅ Working |
| Section breaks, different first/odd/even page headers, images, tab stops | ❌ Not started |

## Architecture

One process. QML's `TextEdit`, backed directly by a `QTextDocument` — no bitmap tiles, no IPC, no serialization:

```
┌───────────────────────────────────────────────┐
│                  rune (single process)         │
│                                                  │
│   Main.qml ── TextEdit (QTextDocument-backed)   │
│      │                                          │
│      ▼                                          │
│   DocumentController                            │
│      │        │          │                      │
│      ▼        ▼          ▼                      │
│  OdfReader  (QTextDocument-  DocxBridge          │
│  (custom)    Writer, Qt's)  (soffice, batch-only)│
└───────────────────────────────────────────────┘
```

**This replaced an earlier two-process LibreOfficeKit design** (kept in [`legacy-lok/`](legacy-lok/) for reference). That architecture's tiled bitmap rendering had a real performance ceiling (~150ms per full-page re-render on a page with a table), and its socket/JSON-protocol split existed specifically to isolate LibreOfficeKit's thread-safety and shutdown issues. `QTextDocument` has neither problem — it's a normal in-process Qt object — so the architecture collapsed to one process once we moved to it.

**Qt has no built-in ODF reader**, public or private — only a writer. `OdfReader` (in `native/formats/`) is a from-scratch ODF importer: unzips via `QuaZip`, parses `content.xml`/`styles.xml` via `QXmlStreamReader`, resolves named/inherited paragraph styles, and builds the document via `QTextCursor`. Verified against real LibreOffice-authored files, not just round-tripped through our own writer.

**`.docx` is a batch conversion at the boundary, not a live format.** Opening a `.docx` runs `soffice --headless --convert-to odt` once, then loads the result with the same `OdfReader`; saving does the reverse. LibreOffice never touches the interactive editing path, and the conversion runs on a background thread (`QtConcurrent`) so it never blocks the UI — empirically verified, not just assumed (a synchronous baseline blocked the main thread completely; the async version let other work run throughout an equivalent-length conversion).

**Pages come from `QTextDocument` itself.** The body is paginated at the paper size, with the margins as the root frame's, so text never runs into a margin; the page sheets in the view are drawn behind it. The header and footer are documents of their own, drawn on every page with that page's number. Qt's writer produces no `styles.xml`, so Rune adds one holding the page layout and the master page's header and footer, with real page-number fields; LibreOffice's `.docx` conversion of it is checked in the tests (page size, margins, header text, `PAGE`/`NUMPAGES` fields).

**Qt's own ODF writer has a real bug**, found and worked around: a table immediately following a list produces structurally broken ODF (not malformed XML — `xmllint` passes it — but a table nested inside the preceding list and split across its own rows), and LibreOffice silently drops the table on import. Fixed with a save-time sanitizer that inserts a separating paragraph; verified with a direct A/B test (identical document, sanitizer on vs. off) showing the unsanitized version genuinely loses the table.

Full protocol/implementation notes: [`native/`](native/) source comments (each file documents its own tested limitations) and [TODO.md](TODO.md) for the complete, current feature state.

## Install

Packages for Arch/Omarchy, Debian 13 and Fedora 42 are attached to each [GitHub release](https://github.com/ninepointlabs/rune/releases/latest).

**With [autonomix-cli](https://github.com/ninepointlabs/autonomix-cli)**, which picks the right package for your system and keeps Rune updated from GitHub Releases:

```sh
autonomix-cli add https://github.com/ninepointlabs/rune
```

**Or download the package for your distro** from the [latest release](https://github.com/ninepointlabs/rune/releases/latest) and install it:

```sh
sudo pacman -U rune-*-x86_64.pkg.tar.zst      # Arch / Omarchy
sudo apt install ./rune_*_amd64.deb           # Debian 13
sudo dnf install ./rune-*.x86_64.rpm          # Fedora 42
```

Each package is built against that distro's own Qt, so use the one that matches your system. Install LibreOffice Writer too if you want to open and save `.docx`/`.doc`.

## Building

Requires Arch/Omarchy with Qt 6.7+, QuaZip, and LibreOffice (for the `.docx` bridge only — not needed to edit native `.odt` files):

```sh
sudo pacman -S cmake ninja qt6-base qt6-declarative quazip-qt6 libsecret openssl libreoffice-fresh

git clone https://github.com/ninepointlabs/rune.git
cd rune
cmake -S . -B build -G Ninja
cmake --build build
```

## Running

```sh
./run.sh                      # blank document
./run.sh path/to/doc.docx     # open a specific file (.odt, .docx, or .doc)
```

## Testing

```sh
QT_QPA_PLATFORM=offscreen ./build/native/rune --auto-test
```

On Arch, Qt sends its warnings to the systemd journal when stderr isn't a terminal; add `QT_FORCE_STDERR_LOGGING=1` to see QML warnings in the test output.

399 checks, run headlessly: typing, formatting, tables, lists, pages and Page setup, headers and footers (double-click to edit, fields, formatting), the File menu and autosave, save/load round-trips through real `soffice` conversion (not just "the code said it succeeded" — e.g. the `.docx` round-trip is independently re-verified by converting the saved file back to text with real LibreOffice and diffing the content), and an empirical proof that `.docx` conversion doesn't block the UI thread (a background ticker's fire count during conversion, not just an API shape check).

## Contributing

Before relying on a Qt or ODF API behaving the way its documentation suggests, verify it against a real file first — this project has twice found real, silent-failure bugs (Qt's ODF writer mangling list-then-table structure; a Unix-socket LOK engine's commands that looked valid but silently did nothing) that only surfaced by testing actual output, not by reading documentation. Every `native/` source file documents what it verified and how.

See [TODO.md](TODO.md) for what's not built yet.

## License

MIT — see [LICENSE](LICENSE).