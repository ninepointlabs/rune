#pragma once

// OdfWriter: a document as an .odt file's bytes.
//
// The body is written by Qt's QTextDocumentWriter (format "ODF") from a
// sanitized copy (see sanitize()); Qt's writer produces no styles.xml, so
// the page layout, header and footer are added as one afterwards, and the
// document's data stores (DocumentData) as package entries
// "Rune/<name>.json". Every added entry is listed in the manifest, as ODF
// requires of a package's files.

#include "DocumentData.h"

#include <QByteArray>

class QTextDocument;

namespace OdfWriter {

// Rewrites `doc` so QTextDocumentWriter's ODF output is well-formed. Every
// ODF save must go through this (write() does); see the .cpp for each rule.
void sanitize(QTextDocument *doc);

// Where a data store goes in the package.
QString dataEntryName(const QString &store);

// The .odt for `body`, with `stylesXml` as its styles.xml (see
// PageLayout::odfStyles()) and `data`'s stores. `body` itself is not
// changed. Empty on failure, with a warning logged.
QByteArray write(const QTextDocument *body, const QByteArray &stylesXml, const DocumentData::Stores &data = {});

} // namespace OdfWriter
