#pragma once

// OdfReader: loads an ODF text document (.odt) into a QTextDocument. Qt has
// no ODF reader; this walks content.xml with QXmlStreamReader.
//
// Stage 4a scope: paragraphs, headings (<text:h>) and bold / italic /
// underline from content.xml's automatic styles. Not read yet: named styles
// in styles.xml (a paragraph whose heading-ness or formatting comes only from
// its named style loads as a plain paragraph), list and table structure (their
// paragraphs load as plain paragraphs, in document order), alignment, fonts,
// colors, images, notes and annotations (skipped).

#include <QString>

class QTextDocument;

namespace OdfReader {

// Replaces the contents of `doc` with what was read from `path`. Returns
// true on success; on failure `doc` is not touched and `*error` (if
// non-null) gets a human-readable message. Clears the undo history.
bool readInto(QTextDocument *doc, const QString &path, QString *error = nullptr);

} // namespace OdfReader
