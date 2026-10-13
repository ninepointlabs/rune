#pragma once

// OdfTextWriter: a QTextDocument's text as ODF block elements, with the
// automatic styles they use. The body of content.xml, and a header's or
// footer's in styles.xml.
//
// Written:
//  - paragraphs, and headings as <text:h> with their outline level;
//    alignment, margins and first-line indent, page breaks;
//  - bold, italic, underline, strikethrough, super/subscript, font family
//    and size (a heading's relative size as the size it is drawn at),
//    color and highlight; links as <text:a>;
//  - tabs, runs of spaces and line breaks as their elements;
//  - inline objects as their kind writes them (InlineObjects);
//  - lists, nested by level, bullet or numbered, a list interrupted and
//    resumed continuing its numbering;
//  - tables: width, borders, padding, cell spans and backgrounds, and any
//    content in cells, nested tables included.
// Every paragraph style also carries the document's default font, which
// unformatted text is in, so the text looks the same elsewhere.
// Not written: images (Rune has none).

#include "DocumentData.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QTextFrame>

class QTextBlock;
class QTextCharFormat;
class QTextDocument;
class QTextList;
class QTextTable;

class OdfTextWriter
{
public:
    struct Options
    {
        // Automatic style names start with this: unique per document
        // written into the same file (the header's and footer's share
        // styles.xml).
        QString stylePrefix;
        // The text area's width: tables' widths are relative to it.
        qreal textWidthMm = 165.1;
        // Extra children of every paragraph style's
        // <style:paragraph-properties> (the header's and footer's tab stops).
        QString paragraphProperties;
    };

    explicit OdfTextWriter(const Options &options) : m_options(options) {}

    // The namespace declarations every element written uses, for the root
    // element of the file they go in.
    static QString namespaces();

    // The named styles what write() writes refers to, for styles.xml's
    // <office:styles>: "Heading 1" to "Heading 6", which headings' styles
    // inherit, so other programs see them as headings (LibreOffice's
    // Navigator, Word's Heading 1 to 6). They set no formatting: a
    // heading's look is its own.
    static QString namedStyles();
    static QString headingStyleName(int level);

    // `doc`'s blocks.
    QString write(const QTextDocument *doc);
    // The automatic styles what write() wrote uses, so far.
    QString automaticStyles() const { return m_styles; }

private:
    struct OpenList
    {
        QTextList *list;
        int level;
    };

    QString writeFrame(QTextFrame *frame);
    QString writeFrameRange(QTextFrame *frame);
    QString writeRange(QTextFrame::iterator it);
    QString writeBlock(const QTextBlock &block);
    QString writeTable(QTextTable *table);
    QString paragraphStyle(const QTextBlock &block);
    QString textStyle(const QTextCharFormat &format);
    QString listStyle(QTextList *list);
    QString textProperties(const QTextCharFormat &format) const;

    Options m_options;
    const QTextDocument *m_doc = nullptr;
    QString m_defaultText; // <style:text-properties> attributes of the default font
    QString m_styles;
    QHash<QString, QString> m_paragraphStyles, m_textStyles; // properties -> name
    QHash<QTextList *, QString> m_listStyles;
    QSet<QTextList *> m_listsWritten; // for continuing their numbering
    int m_tables = 0;
};
