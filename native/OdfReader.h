#pragma once

// OdfReader: loads an ODF text document (.odt) into a QTextDocument. Qt has
// no ODF reader; this walks styles.xml and content.xml with QXmlStreamReader.
//
// Read: paragraphs, headings (<text:h>); bold / italic / underline, font
// family, size and color, and paragraph alignment, from both content.xml's
// automatic styles and styles.xml's named styles, following
// style:parent-style-name chains; bullet and numbered lists, nested by level,
// built the way DocumentController builds them; tables, including cell spans
// and multi-paragraph and nested-table cells, in DocumentController's table
// format.
//
// Not read: a paragraph whose heading-ness comes only from its named style
// gets the style's look (size, weight) but no heading level; styles.xml's
// default styles (style:default-style) and its own automatic styles
// (headers/footers); percentage font sizes; list numbering restarts and
// start values, list headers (read as ordinary items), bullet characters and
// number formats (lists get the UI's styles for their level); table and
// column widths, cell borders and backgrounds; images, notes and
// annotations (skipped).

#include <QHash>
#include <QString>
#include <QTextFormat>

class QTextDocument;

namespace OdfReader {

// One <style:style>: the properties it sets itself, and the style it
// inherits the rest from.
struct Style
{
    QString parent; // style:parent-style-name, same family
    QTextCharFormat charFormat; // <style:text-properties>
    QTextBlockFormat blockFormat; // <style:paragraph-properties>
};

// Styles by family ("paragraph", "text") and name.
class StyleSheet
{
public:
    // Replaces any style of that family and name.
    void insert(const QString &family, const QString &name, const Style &style);
    bool contains(const QString &family, const QString &name) const;
    // The named style with its ancestors' properties merged in, the nearer
    // style winning where both set one; `parent` is left empty. The chain
    // ends at a missing parent or at a style already visited (an
    // inheritance loop), keeping what was resolved up to there. An unknown
    // name resolves to an empty style.
    Style resolve(const QString &family, const QString &name) const;

private:
    QHash<QString, Style> m_styles;
};

// Replaces the contents of `doc` with what was read from `path`. Returns
// true on success; on failure `doc` is not touched and `*error` (if
// non-null) gets a human-readable message. Clears the undo history.
bool readInto(QTextDocument *doc, const QString &path, QString *error = nullptr);

} // namespace OdfReader
