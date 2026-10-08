// Minimal Markdown -> HTML for seeding Writer documents via paste.
//
// Supports ATX headings, paragraphs, **bold**/__bold__, *italic*/_italic_,
// unordered/ordered lists (flat), fenced code blocks, `inline code`,
// [links](url) and horizontal rules. Anything else passes through as text.

#pragma once

#include <string>

// Returns a complete HTML page (UTF-8 charset declared) for the given
// UTF-8 Markdown source.
std::string mdToHtml(const std::string &md);
