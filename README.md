# Rune

An AI-native word processor for Linux/Omarchy. Opens and edits `.docx`, `.odt`, `.txt`, and `.md` with high fidelity. Built-in multi-provider LLM integration (Claude, ChatGPT, Grok) via OAuth — invoke any model for inline edits, drafts, or idea generation without leaving the editor.

## Architecture

Qt/QML frontend → C++ bridge → LibreOfficeKit (headless document engine) + AI Manager (multi-provider OAuth + streaming)

Full architecture plan: see [docs/architecture.md](docs/architecture.md)

## Status

Pre-development. Next step: spike the LibreOfficeKit tiled rendering API to validate the core architecture.