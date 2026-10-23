# Rune — Roadmap

Status as of 2026-10-19 (Rune 0.8.6). See [README.md](README.md) for the architecture overview.

## Done (in `native/`, the current app)

- Document I/O: `.odt` native read/write; `.docx`/`.doc` via a `soffice` batch-conversion bridge, async (doesn't block the UI), verified against real-world files not just our own writer's output
- Full text editing: keyboard, mouse, clipboard — all native `TextEdit` behavior, not custom-built
- Formatting: Bold/Italic/Underline, paragraph styles with named/inherited style resolution on import (`styles.xml` + `content.xml` automatic-styles, parent-style-name chains), bullet/numbered lists with promote/demote nesting, alignment, font family/size, text color
- Tables: insert, insert/delete row, insert/delete column, Tab/Shift+Tab cell navigation
- Rune's own ODF writer (`native/formats/OdfWriter`, `OdfTextWriter`): headings as headings (Word's Heading 1–6 after conversion), links, strikethrough, super/subscript, lists that keep numbering across interruptions, tables with widths, borders, merged cells and nested tables. It replaced Qt's writer, which dropped heading levels and nested tables and broke a table placed right after a list (still shown by an A/B check against Qt's writer)
- File flow: Open (Ctrl+O), Save (Ctrl+S), Save As (Ctrl+Shift+S), New (Ctrl+N) with native file dialogs (GTK via the platform theme on Omarchy; Qt's own as a fallback). Save writes to the current file when it's `.odt`/`.docx` and goes through Save As otherwise (untitled, or opened from `.doc`). New/Open/close with unsaved changes ask Save / Discard / Cancel. The editor is read-only while a `.docx`/`.doc` converts
- Undo/redo: toolbar buttons, Ctrl+Z / Ctrl+Shift+Z. Every formatting, list and table operation is exactly one step (tested per operation); undoing back to the last save makes the document clean again; New and Open clear the history. Typing undoes a word at a time (a word plus its trailing spaces); moving the cursor, Backspace/Delete, Return, typing over a selection, or saving also starts a new step. Qt on its own merges all consecutive typing into one step, so the controller inserts typed text itself to control the grouping
- Pages: the document is laid out on page-sized sheets at its paper size and margins (Letter in Letter countries, A4 elsewhere; 1 inch margins), scaled down to fit a narrow window; "Page N of M" in the status bar. File → Page setup… sets paper (Letter, Legal, A4, A5), orientation and margins (inches or centimetres to match the paper), one undo step. The layout is saved in `styles.xml` and read back from `.odt` and `.docx`; LibreOffice's `.docx` of a saved file has the same page size and margins (checked in the tests)
- Headers and footers: double-click a page's top or bottom margin (or File → Edit header / Edit footer, Ctrl+K) to edit in place. The toolbar and Ctrl+B/I/U format them meanwhile; # Page and # Pages insert page number and page count fields, each page drawing its own; Ctrl+K → Page numbers adds a centred one to the footer. Centred in their margins; one that grows too tall moves the body down (one undo step). Escape, Done or a click on the page goes back to the body. Saved in `styles.xml`'s master page with real fields, so Word and LibreOffice show them in the same place (`PAGE`/`NUMPAGES` in the `.docx`); read back from `.odt` and `.docx`
- Command palette (Ctrl+K, File → Command palette…): every command, searchable, with its shortcut; ones that don't apply right now are listed dimmed
- About Rune (File → About Rune, Ctrl+K → About): version, license (readable in place), support address with Email and Copy, privacy, toolkit versions
- Author tooling, one file per book:
  - **Headings:** a paragraph style menu on the toolbar (Body, H1–H3; Ctrl+Alt+0–3). Chapters are Heading 1 and scenes Heading 2; Return after a heading starts body text.
  - **Outline:** the sidebar (☰, Ctrl+K → Show outline) lists chapters and scenes with word counts. A click goes to one; dragging one by its handle moves the whole section, scenes and all, as one undo step. The total word count is also in the status bar.
  - **Story notes:** characters, places and research in the sidebar's Notes tab, saved in the file (`Rune/notes.json` in `.odt`, a custom XML part in `.docx`).
- Citations:
  - **Sources:** CSL-JSON in the sidebar's Sources tab. Add or edit them in a form, or import BibTeX.
  - **Citing:** Insert citation (Ctrl+Shift+C) cites one or more sources with an optional page. Citations are drawn in the chosen style: APA 7, MLA 9, Chicago author-date or IEEE (numbered by first citation).
  - **Bibliography:** Insert bibliography writes a "References"/"Works Cited" section that follows every change to what's cited, the sources or the style. A citation deleted with the keyboard takes its entry with it, and one undo brings both back.
  - **Saving:** each citation is saved as its text between hidden `_RuneCite` bookmarks, with what it cites in the file's data. Other programs show the text, and the citations are live again when reopened, through `.docx` too. The bibliography uses the "Bibliography Heading"/"Bibliography 1" paragraph styles.
- Agents and story data:
  - The AI is sent the notes and sources as JSON, sees citations as Pandoc `[@key]`, and cites the same way; `[@key]` in its text becomes a citation when the key is a source.
  - It adds sources and updates notes in fenced data blocks, which never reach the text.
  - Sources it adds are marked "added by AI · check it" until reviewed, and their DOI or URL is checked: a link that doesn't resolve is flagged as broken.
- `.docx` round-trips verified against `samples/test.docx`/`test.doc` (real files, not generated by this codebase) with real `soffice` conversion as the independent check

## Known limitations — found, tested, and documented, not guessed

| Limitation | Why |
|---|---|
| **List numbering restart, custom start values, bullet characters, number formats** | Imported lists always get the UI's own level styles rather than preserving the source document's exact numbering presentation. |
| **Table/column widths, cell borders, backgrounds** | Written (table width, border, padding, cell backgrounds), not read: opened tables get the UI's table format. Column widths are neither. |
| **`styles.xml`'s default-style** | Not read — named paragraph styles with `style:parent-style-name` chains are, and the automatic styles headers and footers use. |
| **One header and one footer per document** | A file's different first-page or left-page header is not shown, and is dropped on save. Only the master page named Standard (or else the first) is read. |
| **Story data and undo** | Changing notes or sources is not an undo step, and undoing an AI edit doesn't take back notes or sources it added. Changes to the bibliography do follow undo. |
| **Rune's data in other programs** | LibreOffice and Word show citations and the bibliography as text. The links back to sources, the notes and the sources live in Rune's data (package entries in `.odt`, a custom XML part in `.docx`). If LibreOffice re-saves an `.odt`, it is expected to drop Rune's entries (not verified), and the citations would become plain text. Word is documented to keep custom XML parts (not verified). |
| **Editing a citation** | A citation can't be edited in place yet: delete it and insert it again. The page reference is a page number only (no chapter or section locators). |
| **Header and footer placement** | Rune centres them in their margins and saves where that puts them; a file's own header distance only decides where the body starts. Editing them has its own undo history, separate from the body's. |

## Not started

| Feature | Notes |
|---|---|
| **Images** | No insertion, no text wrap, no import/export at all |
| **Header & footer variants** | "Different first page," "different odd/even," "link to previous" |
| **Section breaks** | Mixed page orientation, independent headers/margins per section, restart page numbering |
| **Tab stops & leader lines** | Needs a ruler widget (headers and footers have fixed centre and right stops) |
| **Table of Contents** | The outline navigator is built; a generated table of contents in the text is not |
| **Exact paragraph spacing / keep-together / widow-orphan** | Was a documented LibreOfficeKit limitation in the old architecture; not yet investigated against `QTextBlockFormat`'s own capabilities in the new one — may actually be achievable now since we're not routing through `.uno:` commands anymore. Worth a fresh look. |
| **Find/replace** | Not built |
| **Spell check** | Not built |

## AI integration (the actual differentiator)

**Built: the provider layer** (`native/ai/`, the AI panel in `native/AiPanel.qml`):

- **Sign in with ChatGPT** (OpenAI's open-source plan-usage program): ChatGPT Plus/Pro users authorize Rune in the browser and requests draw on their existing plan. Full spec: dynamic client registration, PKCE + state + nonce, ID token verified (RS256 against OpenAI's JWKS), rotating refresh, revoke on sign-out, OpenAI's error codes and required UI wording ("Using ChatGPT plan", "Usage limit reached → Manage usage", the first-sign-in notice)
- **OpenRouter** (OAuth PKCE → a user-controlled key): one sign-in reaches Claude, GPT and Grok models, paid from the user's OpenRouter credits
- Secrets in the desktop keyring (libsecret); streaming over SSE; model picker; per-provider model remembered
- **Edit document** mode (Ctrl+Shift+E), one undo step per instruction; the editor is read-only while the AI writes; a failure reverts the document exactly, Stop keeps what was done. The model answers in one of two forms:
  - **Changes to existing text** (spelling, grammar, wording, spacing): find/replace blocks, each applied as it arrives, within the selection if there is one, else anywhere in the document. Only the characters that differ are rewritten, so formatting survives; quotes with Markdown markup or different spacing still match, and so does a long quote with a stray space or curly quotes; changes whose text isn't found are skipped and reported, never guessed. A blank line in a replacement makes a real empty paragraph (taken out of any list). The document goes to the model one paragraph to a line (Qt's Markdown wraps at 80 columns, and models copied the wraps into replacements as paragraph breaks); line breaks a reply still puts inside one paragraph are taken as spaces
  - **New text**: replaces the selection or is written at the cursor, streamed in live, then converted from Markdown to real formatting (headings, lists, emphasis, tables; `&nbsp;` lines become empty paragraphs). Multi-paragraph replies get paragraphs of their own; a one-phrase rewrite stays inline
- **Ask** mode: questions and research in the panel, with the document and selection sent as context (each side of the selection cut to 60k characters)

**Why not "OAuth for Claude, ChatGPT, Grok"** (the original plan), checked 2026-10-09:
- **Claude**: Anthropic's terms prohibit third-party apps from offering Claude.ai login or using Free/Pro/Max credentials ([legal & compliance](https://code.claude.com/docs/en/legal-and-compliance)); apps must use API keys. Rune reaches Claude through OpenRouter instead
- **Grok**: xAI has no third-party OAuth program; tools that offer it borrow xAI's own client. Not built on. Grok is available through OpenRouter

**Next:**
- [ ] Formatting changes by instruction ("make every heading blue", "bold the names"): changes are text-only; formatting comes from the toolbar
- [ ] A rewrite of a whole selection as *new text* still goes through Markdown, losing what Markdown can't express (colors, alignment, fonts) in that selection
- [ ] An inline prompt at the selection (instead of the side panel)
- [ ] New text takes default formatting, not the surrounding paragraph's (fonts, colors)
- [ ] Optional API-key entry for people who want to use Anthropic or xAI directly
- [ ] OpenAI's official "Continue with ChatGPT" button artwork (currently a plain text button)
- [ ] Not verified by tests: a real sign-in against OpenAI's and OpenRouter's servers (tests use local mock servers; the real token/key endpoints were checked to be reachable and to answer as documented)

## Architecture notes for contributors

- `native/` is the current app — single process, `QTextDocument`-based. `DocumentController` is the QML-facing API, delegating to the editing model in `native/document/` (formatting, lists, tables, streamed edits, header/footer, page layout, the outline, story notes, sources and citations); `native/formats/` (`OdfReader`, `OdfWriter`, `DocxBridge`) is plain C++ (not QML-exposed) handling import, export and the `.docx` bridge. `Main.qml` holds the window's state and file flow, with `Toolbar.qml`, `PageView.qml`, `StatusBar.qml`, `Commands.qml` and the sidebar (`Sidebar.qml`: `OutlinePanel`, `NotesPanel`, `SourcesPanel`) beside it; the `--auto-test` checks are in `native/tests/`, one file per feature. See [docs/architecture-plan.md](docs/architecture-plan.md) for where this is heading.
- `legacy-lok/` is the **retired** two-process LibreOfficeKit implementation, kept for reference only. See `legacy-lok/README.md`.
- Run `build/native/rune --auto-test` (headless, `QT_QPA_PLATFORM=offscreen`) after any change — 504 checks covering formatting, tables, lists, undo/redo, pages and Page setup, headers and footers, the command palette, the file Open/Save flow, the File menu and autosave, both AI sign-in flows and AI document edits against mock servers, save/load round-trips verified via real `soffice` conversion, and an empirical proof the `.docx` bridge doesn't block the UI thread. On Arch, Qt sends warnings to the systemd journal when stderr isn't a terminal: run with `QT_FORCE_STDERR_LOGGING=1` to see QML warnings (a null binding once drew every header blank with nothing in the test log).
- **Verify Qt/ODF API behavior against real files before relying on it.** This project has twice found real, silent-failure bugs this way: Qt's ODF writer mangling list-then-table structure (confirmed via `xmllint` + a direct A/B `soffice` conversion test), and earlier, the old LOK engine's `.uno:` commands that returned `ok` while silently doing nothing (confirmed via tile-diffing before/after). Don't assume an API's documented behavior is what actually happens on disk — test it.