#pragma once

// OdfWriter: a document as an .odt file's bytes, written by Rune itself
// (Qt's QTextDocumentWriter wrote malformed ODF in places, and can't write
// headings, notes or fields):
//  - content.xml: the body (OdfTextWriter);
//  - styles.xml: the page layout, header and footer (OdfStyles);
//  - meta.xml: the generator, Rune and its version;
//  - Rune/<name>.json: the document's data stores (DocumentData);
//  - the mimetype first and uncompressed, and a manifest listing it all,
//    as ODF requires.

#include "DocumentData.h"
#include "PageLayout.h"

#include <QByteArray>
#include <QString>

class QTextDocument;

namespace OdfWriter {

struct Document
{
    const QTextDocument *body = nullptr;
    // With the header's and footer's distances where they are drawn.
    PageLayout layout;
    // None if null or empty.
    const QTextDocument *header = nullptr;
    const QTextDocument *footer = nullptr;
    DocumentData::Stores data;
};

// Where a data store goes in the package.
QString dataEntryName(const QString &store);

// The .odt. Empty on failure, with a warning logged.
QByteArray write(const Document &document);

} // namespace OdfWriter
