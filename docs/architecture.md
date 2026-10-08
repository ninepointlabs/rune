# Rune — Initial Architecture Plan

**Status:** Draft
**Date:** 2026-10-08

## Overview

Rune is a Linux-native word processor that combines LibreOfficeKit's document engine with a modern Qt/QML frontend and multi-provider AI integration. The goal is Word-comparable editing with a built-in LLM assistant as the primary differentiator.

## Architecture

```
┌──────────────────────────────────────────────┐
│                  QML Frontend                 │
│  ┌──────────┐  ┌──────────┐  ┌────────────┐  │
│  │ Document  │  │   AI     │  │  Settings   │  │
│  │  Canvas   │  │ Sidebar  │  │   Panel     │  │
│  └─────┬─────┘  └────┬─────┘  └────────────┘  │
│        │              │                        │
├────────┼──────────────┼────────────────────────┤
│        │         C++ Bridge                     │
│  ┌─────┴──────────────┴─────────────────────┐  │
│  │          Rune Engine (C++)                │  │
│  │  ┌──────────────────┐  ┌──────────────┐  │  │
│  │  │  LibreOfficeKit  │  │  AI Manager   │  │  │
│  │  │  (headless)      │  │  (OAuth +     │  │  │
│  │  │                  │  │   streaming)  │  │  │
│  │  └──────────────────┘  └──────────────┘  │  │
│  └───────────────────────────────────────────┘  │
└─────────────────────────────────────────────────┘
```

### Document Engine — LibreOfficeKit

LibreOffice runs headless as a persistent daemon. The C++ bridge manages one LO process for all open documents, communicating via the LibreOfficeKit API:

- **Document I/O:** Open/save `.docx`, `.odt`, `.rtf`, `.txt` through LO's import/export filters. Binary `.doc` support comes later (Phase 4).
- **Layout rendering:** LO's tiled rendering API paints document pages into bitmap tiles, which the QML canvas composites and displays. GPU-accelerated via Qt Quick Scene Graph.
- **Text editing:** Keystrokes and formatting commands are forwarded to LO; the bridge re-renders affected tiles on change.
- **Cursor/selection:** LO reports cursor position and selection ranges; the QML canvas overlays native-looking cursors and highlights.

**Why LO over a custom engine:** Format fidelity for `.docx` is a multi-year problem. LibreOffice has solved most of it. Building on LO lets Rune ship a working editor in months rather than years.

**Markdown handling:** LO's built-in markdown export is weak. Rune will implement a custom markdown importer/exporter that maps markdown structures to LO's internal document model for import, and traverses LO's document model to produce clean markdown on export.

### AI Integration — Multi-Provider OAuth

The AI Manager handles provider connections:

- **OAuth flow:** Each provider (Anthropic, OpenAI, xAI) gets a standard OAuth 2.0 authorization flow. Tokens are stored encrypted in the system keychain via Omarchy's secrets API.
- **Token lifecycle:** Automatic refresh, with re-authorization prompts when refresh fails.
- **Context assembly:** Selected text + surrounding paragraphs (up to a configurable token budget) + document metadata (title, style names, word count). Sent as system/user message pairs to the provider's chat API.
- **Streaming:** Responses stream character-by-character into the document or sidebar. User can cancel mid-generation.
- **Provider selection:** Per-document preference stored in document metadata. Global default in settings. Quick-switch in the AI sidebar.

### Interaction Modes

1. **Inline edit:** User highlights text, presses a shortcut (e.g. Ctrl+Shift+E), types an instruction ("make this more concise"), and the AI rewrites the selection in place. User accepts (Enter) or rejects (Escape).

2. **Sidebar chat:** A persistent panel docked to the right of the document. Full conversation thread with document context. The AI can reference, quote, and suggest edits to the document from the sidebar. User can insert generated text at the cursor with one click.

3. **Draft generation:** User places the cursor and invokes "Draft" (Ctrl+Shift+D), describes what they want, and the AI generates text inline. User accepts or rejects each paragraph as it arrives.

### Performance Strategy

- **Persistent LO daemon:** LO starts once and stays resident. Document open is near-instant after the first launch — no cold-start penalty.
- **Tiled rendering:** Only visible tiles are rendered. LO's tiled rendering API is designed for this — it skips off-screen content automatically.
- **Virtualized canvas:** The QML ListView/Flickable approach — only tiles in the viewport are painted. Large documents (100+ pages) scroll without allocating memory for the entire document at once.
- **Async I/O:** File open/save runs off the main thread. The UI stays responsive during large document loads.
- **Warm start:** On app launch, the LO daemon starts immediately in the background. By the time the user opens a document, the engine is ready.

## Feature Roadmap

### Phase 1 — Core (MVP)

- [ ] Open/save `.docx`, `.odt`, `.txt`, `.md`
- [ ] Basic formatting: bold, italic, underline, headings (H1-H3), bulleted/numbered lists
- [ ] Text selection, cursor navigation, cut/copy/paste
- [ ] Dark/light theme via Omarchy theming system
- [ ] Single AI provider: Claude (Anthropic OAuth)
- [ ] Inline AI edit mode
- [ ] Persistent LO daemon with warm start
- [ ] Omarchy panel integration (bar widget for quick access)

### Phase 2 — AI Deep

- [ ] Multi-provider OAuth: Claude, ChatGPT, Grok
- [ ] AI sidebar chat panel with conversation history
- [ ] Draft generation mode
- [ ] Per-document model preference
- [ ] Context window controls (token budget, include/exclude sections)
- [ ] AI action history (undo/redo AI edits)

### Phase 3 — Formatting & Layout

- [ ] Tables (create, edit, resize)
- [ ] Images (embed, resize, align)
- [ ] Paragraph styles and document themes
- [ ] Track changes (visual markup, accept/reject)
- [ ] Comments (marginal annotations)
- [ ] Headers, footers, page numbers
- [ ] Page layout controls (margins, orientation, columns)

### Phase 4 — Advanced

- [ ] Binary `.doc` support (via LO's import filter — expect some fidelity loss on complex documents)
- [ ] PDF export
- [ ] `.rtf` read/write
- [ ] Real-time collaboration primitives (document diffing, merge hints)
- [ ] Plugin system for custom AI providers and document processors
- [ ] Spell check and grammar (with AI-assisted suggestions)
- [ ] Find and replace (with regex and AI-powered "smart replace")

## Technical Risks & Mitigations

| Risk | Severity | Mitigation |
|------|----------|------------|
| LibreOfficeKit tiled rendering API is underdocumented | High | Spike first. Build a minimal prototype that renders a page to QML tiles before committing to the full architecture. |
| Binary `.doc` format fidelity will always be lossy | Medium | Set expectations clearly. Flag `.doc` as "legacy — open a copy." Push users toward `.docx`. |
| LO process management (crashes, memory leaks, clean shutdown) | Medium | Run LO in a subprocess with a health watchdog. Auto-restart on crash. Cap memory and kill/restart after N documents opened. |
| Multi-provider OAuth token lifecycle complexity | Low | Each provider is an isolated module with a standard interface. Start with one, add more when the pattern is proven. |
| Markdown round-trip fidelity | Medium | Design the markdown exporter/importer as a standalone module with its own test suite. LO's internal document model is the canonical representation; markdown is a serialization. |
| QML canvas performance with complex documents | Medium | Benchmark with real-world 100+ page `.docx` files. If tiled rendering via LO is too slow for scrolling, explore pre-rendering full pages to GPU textures. |

## Next Actions

1. **Spike: LibreOfficeKit tiled rendering.** Build a minimal C++ program that opens a `.docx` via LO, renders a page tile, and displays it in a QML window. This validates the core architecture before any feature work begins.
2. **Scaffold the repo.** Create CMake build, basic QML shell, and the LO bridge skeleton.
3. **Research LO documentation.** Catalog the LibreOfficeKit API surface: which calls are stable, which are experimental, and where the documentation gaps are.