# Rune — Modularity Plan

**Status:** All four stages done
**Date:** 2026-10-13

Rune will grow features that cut across the whole editor: citations and a
bibliography, footnotes and endnotes, and author tooling (chapters,
characters, research notes). This plan reshapes the code so each of those
can be added as a module of its own instead of another section of two
already-large files. It replaces nothing in [`architecture.md`](architecture.md),
which describes the retired LibreOfficeKit design and is kept as history.

## Where things stand (Rune 0.8.2)

| Part | Size | State |
|---|---|---|
| `native/ai/` | 2.6k lines | A real module: provider interface, two providers, storage, OAuth, SSE. Talks to the editor through the streamed-edit API. |
| `PageLayout`, `PageParts`, `PagePartView`, `OdfReader`, `DocxBridge` | 0.2–0.8k each | Separate files, one concern each. |
| `DocumentController` | 1.8k lines, 48 invokables, 18 properties | Everything else: file I/O and conversion, character and paragraph formatting, typing and undo, lists, tables, page setup, headers and footers, AI streaming. Every feature so far has landed here. |
| `Main.qml` | 1.5k lines | Toolbar, page view, header/footer editing, command list, shortcuts, dialogs. (Stage 4 split it.) |
| `AutoTest.cpp` | 3.1k lines, 399 checks | One file, one long run. The safety net for everything below. (Stage 4 split it.) |

What blocks the planned features:

1. **One class owns every feature.** Citations and notes would each add
   another few hundred lines and another set of `m_target != m_document`
   guards to `DocumentController`.
2. **Sub-documents are hard-coded.** `EditTarget {Body, Header, Footer}`
   and `m_header`/`m_footer` appear throughout. Footnotes and endnotes are
   the same shape (a text of its own, anchored in the body) and would be a
   fourth and fifth special case.
3. **Custom data has no registry.** Feature data lives on the text as
   numbered Qt properties (`UserProperty+100…107` page layout,
   `UserProperty+120` page fields, `UserObject+1` fields), allocated by hand.
4. **Saving can't grow.** The body is written by Qt's `QTextDocumentWriter`
   and patched afterwards (`sanitizeForOdfExport()`, `withOdfStyles()`
   splicing `styles.xml` into the archive). Qt's writer has no notion of
   notes, bibliography marks or custom fields, so anything it doesn't write
   can't be saved, and `.docx` (LibreOffice's conversion of our `.odt`)
   inherits the same limits. `OdfReader` is a single walker with the
   element handling inline.
5. **The reader depends on the controller.** `OdfReader` calls
   `DocumentController::listStyle()`/`tableFormat()`: the file format layer
   reaches up into the QML-facing class.

## Target structure

```
native/
  main.cpp, *.qml, DocumentController   app layer: the QML facade, views, tests
  PagePartView
  document/                             the editing model, no QML
    EditState                           the documents, edit target, selection,
                                        self-edit guard, content revision
    CharFormatter                       B/I/U, font, size, colour, pending
                                        format, typing undo steps
    Lists, Tables                       list and table structure and edits
    StreamedEdit                        AI replies written into the body
    HeaderFooter                        header/footer documents, fields,
                                        sizes, painting
    PageLayout, PageParts               (moved) page geometry, page fields
  formats/                              reading and writing files
    OdfReader, OdfWriter, DocxBridge
  ai/                                   unchanged
```

Dependencies point one way: `formats/` → `document/`; the app layer → both;
`document/` depends on neither. `ai/` keeps using the controller's
streamed-edit API for now.

## Stages

Each stage is a pure refactor unless it says otherwise: the QML API, file
output and behaviour don't change, and all checks pass on Arch and Debian 13
(`scratchpad/dbg.sh`-style container run) before it is done.

### 1. Split `DocumentController` (done)

Move each feature's logic into `document/` and `formats/` as above.
`DocumentController` stays the single object QML sees, with the same
properties, invokables and signals, and delegates. It keeps what is
genuinely about the open document as a whole: new/open/save, the `.docx`
conversion lifecycle, dirty state and the current path.

Done when: `DocumentController.cpp` is the facade and file lifecycle,
`OdfReader` no longer includes `DocumentController.h`, and 399/399 checks
pass unchanged.

Result: `DocumentController.cpp` went from 1,775 to 778 lines, about half
of it the open/save/conversion lifecycle. New: `document/EditState`,
`CharFormatter`, `Lists`, `Tables`, `StreamedEdit`, `HeaderFooter`;
`formats/OdfWriter`; `PageLayout` stores itself in the root frame
(`fromDocument()`/`storeIn()`). The QML API is unchanged; the only test
change is `sanitizeForOdfExport()` → `OdfWriter::sanitize()`. 399/399 on
Arch and in Debian 13.

### 2. Generalise sub-documents, inline objects and document data (done)

- **Parts registry** (`document/DocumentParts`). The document's texts:
  the body (`""`) and named parts (`header`, `footer`; later
  `footnote:<id>`, `endnote:<id>`), each a `QTextDocument` with a kind.
  The edit target is a part id (`setEditTarget("header")`), and what acts
  there comes from the kind's capabilities (lists, tables, AI, word-sized
  typing undo steps), not from `if (target != body)`. Dirty state and
  undo/redo routing go through the registry. `HeaderFooter` is the module
  for the `PageMargin` kind: it adds its two parts and owns their
  geometry.
- **Inline object registry** (`document/InlineObjects`). Each kind (the
  page fields now) registers its object type, the kinds of part it may
  appear in, its drawing, and its ODF writer and reader. `PageParts::toOdf()`
  and `OdfReader` dispatch through it. `document/FormatIds.h` allocates
  every `UserObject`/`UserProperty` number.
- **Document data** (`document/DocumentData`). Named JSON stores (an
  object or array each) the document carries: the story notes and the
  sources list will be two. They count in the modified state and are saved
  in the file:
  - in `.odt` as `Rune/<name>.json` package entries listed in the manifest
    (`OdfWriter`, `OdfReader::readData()`);
  - in `.docx` as one custom XML part (`formats/DocxData`), added to
    LibreOffice's output and read from the `.docx` before converting.
  `formats/Package` reads and writes the zip archives; it replaced
  `withOdfStyles()`.

QML API changes: `setEditTarget()`, `setPartDocument()` and
`insertPageField()` take part ids, `paintPart()` too, and `PagePartView`
has `part: "header"` instead of `footer: false`.

Result: 421 checks (22 new) on Arch and in Debian 13. The new checks
cover the registries and the data stores: saved and reopened in both
formats, LibreOffice still opening both files, a store that isn't JSON
skipped. Found on the way: a new controller reported its header and footer
as modified (setting their margin is a recorded format change). It never
showed in the app, which calls `newDocument()` at startup; new parts now
start clean.

Not yet, by design:
- **Markdown for the AI** in the inline object registry arrives with the
  first kind that appears in the body (citations, `[@key]`), together with
  the parsing of replies.
- **Data stores aren't in the undo history.** Changing one is not a text
  edit. Revisit if a UI makes accidental changes likely.
- **Not verified:** whether LibreOffice keeps `Rune/*.json` when *it*
  re-saves an `.odt` (likely not, since it rewrites the package), and
  whether Word keeps the custom XML part (documented to). Rune itself
  keeps both.

### 3. Own the ODF output (done)

Rune writes the whole `.odt` itself (`formats/OdfWriter`): mimetype,
manifest, `content.xml`, `styles.xml`, `meta.xml` and the data stores.
Qt's `QTextDocumentWriter` and the `sanitize()` workaround are gone.

- `formats/OdfTextWriter` writes any of the document's texts (the body;
  the header and footer, with their tab stops) as ODF blocks with their
  automatic styles:
  - headings as `<text:h>` with their outline level, inheriting named
    "Heading 1–6" styles, so LibreOffice's Navigator and Word (after
    conversion, `Heading1`…) see them as headings;
  - links, strikethrough and super/subscript;
  - relative sizes written as drawn;
  - lists nested by level, one list resumed after an interruption
    continuing its numbering;
  - tables with their width, borders, padding, cell backgrounds, merged
    cells and nested tables;
  - inline objects through their InlineObjects kind.
- `formats/OdfStyles` has the page layout's ODF, moved from
  `PageLayout`, which is now only the model. `HeaderFooter::asDrawn()`
  gives the distances saving writes.

Result: 431 checks on Arch and in Debian 13. The new checks cover:
- the package's layout;
- the file reopening in Rune with the same text, heading levels and
  list levels;
- LibreOffice reading headings as `<h1>`/`<h2>`, the link, the merged cell,
  a table in a table, and the resumed numbering ("3. Third");
- the `.docx` carrying Word's `Heading1`/`Heading2`.

Two open issues in `TODO.md` are closed: "headings save as bold body text"
and "nested tables lose content on export". One A/B check stays as
evidence: Qt's writer still loses a table placed right after a list.

Still not read back on open: table widths, borders and backgrounds (opened
tables get the UI's format), and list numbering presentation (see
`TODO.md`).

### 4. Split the UI and the tests (done)

- `Main.qml` → `Toolbar.qml`, `PageView.qml` (sheets, header/footer
  hotspots and editors), `StatusBar.qml`, and `Commands.qml` (the
  palette's list, one list per feature).
- `AutoTest.cpp` → `native/tests/`: one file per feature sharing
  `TestSupport.h`'s helpers; still one `--auto-test` run.

Result: `Main.qml` went from 1,457 to 534 lines: the window's state, the
file flow (open, save, autosave, the unsaved-changes guard), header/footer
editing and the shortcuts. The tests' public hooks stay there (the checks
read `editingPart`, `boldActive` and the like, and call `save()`,
`editPart()` and so on). `AutoTest.cpp` is now 54 lines of setup and
order, with `FormattingTests`, `TablesTests`, `OdfTests`, `DocxTests`,
`AsyncIoTests`, `UndoTests`, `PagesTests`, `HeaderFooterTests` and
`WindowTests` beside it; dependencies that used to be implicit between
sections are now explicit (the ODF round-trip text passed to the `.docx`
and async checks, the temp-directory check spanning both in
`AutoTest.cpp`). The same 399 checks run in the same order: 399/399 on
Arch and in Debian 13. One visible change: the palette lists "Turn
autosave on/off" with the other File commands, before the Page ones.

Not done yet: features registering their own toolbar groups. The
toolbar's collapse logic names its groups, so a new feature's group is
still added there by hand.

Stages 1, 2 and 4 are independent enough to interleave; 3 needs 2.

## Decisions

- **Write our own ODF (stage 3)? Done (2026-10-13).** Tim chose to do it
  on its own rather than with the first notes or citations feature. The
  alternative, keeping Qt's writer and patching more into its output, got
  harder with every feature and couldn't express notes inside paragraphs.
- **What author tooling is. Decided (2026-10-13): one file.** The
  criterion is what is most efficient and user-friendly.
  - *User-friendly:* a book is one file the author opens, backs up, emails
    and hands to an editor as `.docx`, with nothing new to learn. Scrivener's
    project-folder model is powerful but is the part of it people find
    confusing (a binder of files, a "compile" step to get a manuscript out,
    sync trouble).
  - *Efficient:* no second model, no project manager, no compile pipeline;
    chapters come from structure the document already has.
  - *Measured:* a 150,000-word, 40-chapter manuscript in one
    `QTextDocument` paginated at Letter size (168 pages, Release build,
    this machine) lays out in ~210 ms. A keystroke mid-book, with full
    re-pagination, takes ~11 ms (worst 15 ms), and moving a chapter takes
    ~50 ms. A typical 80–100k-word novel is well inside a 60 fps frame; much
    longer books will start to feel it, and the mitigation is a later
    "focus on this chapter" view, not a different file model.

  What that means:
  - **Chapters and scenes are headings** (Heading 1 / Heading 2). A
    navigator sidebar lists them: click to jump, drag to reorder (moves the
    whole section, one undo step), with word counts per chapter.
  - **Characters, places and research notes are a store inside the file**:
    an extra entry in the `.odt` package (`Rune/notes.json`, listed in the
    manifest), shown in a sidebar. For `.docx`, the same data goes into a
    custom XML part added after LibreOffice's conversion (as `styles.xml`
    is added to the `.odt` today), and read straight from the `.docx`
    when opening, since LibreOffice's conversion drops it. Word is
    documented to keep custom XML parts, so the notes should survive a
    round trip through an editor's copy of Word (not yet verified).
  - **Agents use the same channels as for citations (below):** chapters
    are Markdown headings in the context they already get, and the notes
    store is sent and edited as a fenced `notes` block of JSON. So an agent
    can add a character it introduced, or update one's description.
  - **Stage 2** gains a third registry beside parts and inline objects:
    document-level data stores (the notes store first), each saving and
    loading itself in `.odt` and `.docx`.
- **Citation format. Decided (2026-10-13): Rune's own sources list in
  CSL-JSON, cited with Pandoc's `[@key]` syntax.** The criterion is what is
  most efficient and user-friendly, *for people and for agents*: an agent
  writing an essay must be able to add its own sources and cite them.
  - *Rejected, ODF bibliography marks (`text:bibliography-mark`):* every
    citation carries a full copy of its source's fields inline, so there is
    no source list to edit, an agent would have to repeat the whole record
    at each citation, and a corrected source would have to be fixed
    everywhere.
  - *Rejected, Zotero/CSL fields:* they need Zotero installed and running,
    which most users don't have and agents can't drive.
  - *Chosen:*
    - **Sources** are a list of records in **CSL-JSON**, the format Zotero,
      Mendeley and Pandoc already exchange and that models know well. They
      are stored in the document (a data store, like the story notes:
      `Rune/sources.json` in the `.odt`, a custom XML part in the `.docx`).
    - **A citation** is an inline object (stage 2) pointing at one or more
      source ids, with an optional locator ("p. 12").
    - **The bibliography** is a generated block: a "References" or "Works
      Cited" section rebuilt from the sources cited.
    - **Styles:** built in, starting with APA 7, MLA 9, Chicago
      author-date and IEEE. Rune implements these itself rather than
      embedding a CSL engine (citeproc-js would mean running a JavaScript
      engine just for this).
  - **For agents, the Markdown they already read and write.** The AI edit
    protocol is Markdown plus FIND/REPLACE blocks (`AiManager::edit()`), so:
    - In the document context sent to the model, a citation appears as
      Pandoc's `[@smith2020, p. 12]` and the existing sources as a fenced
      `sources` block of CSL-JSON, so the model reuses keys rather than
      inventing duplicates.
    - In a reply, the model writes `[@key]` where it cites, and adds any new
      sources in a fenced `sources` block. `StreamedEdit::finish()` turns
      `[@…]` into citation objects and merges the block into the source
      list.
    - Anything that edits the file directly (a future CLI or MCP tool for
      outside agents) reads and writes the same CSL-JSON.
  - **For people:** an Insert citation picker (search the source list), a
    Sources panel to add or edit records by hand, paste BibTeX or a DOI
    (DOI lookup through Crossref, which needs the network and can be
    turned off), and a style menu that re-renders every citation and the
    bibliography.
  - **Sources added by AI are marked unreviewed** until the user confirms
    them, and Rune checks that a DOI or URL resolves where there is one.
    Models invent plausible-looking references, so an agent's sources must
    be visibly checkable.
  - **Other programs see plain text.** In saved files each citation is
    written as its rendered text ("(Smith, 2020, p. 12)") and the
    bibliography as ordinary paragraphs, so Word and LibreOffice show them
    correctly. The links back to the sources live in Rune's data store.
    Someone editing in Word sees text, not live citations. *Not verified
    yet:* how LibreOffice's `.docx` conversion treats the objects; checked
    when stage 3 writes them.

