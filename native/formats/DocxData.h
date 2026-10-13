#pragma once

// DocxData: a document's data stores (DocumentData) in a .docx, as one
// custom XML part. Word keeps custom XML parts it doesn't know (they are
// how add-ins attach data to a document); LibreOffice's conversion drops
// them, so a save adds the part to LibreOffice's output and an open reads
// it from the .docx itself, before converting.
//
// The part: customXml/itemN.xml (the first free N), with its properties
// part and relationships, related from the main document part:
//
//   <rune:data xmlns:rune="urn:ninepointlabs:rune:data:1">
//     <rune:store name="notes">[...JSON...]</rune:store>
//   </rune:data>

#include "DocumentData.h"

#include <QByteArray>
#include <QString>

namespace DocxData {

// `docx` (a whole .docx file) with `stores` added as the part. With no
// stores, `docx` unchanged. Empty if `docx` isn't a .docx this can read.
QByteArray add(const QByteArray &docx, const DocumentData::Stores &stores);

// The stores in the .docx at `path`; none if it has no such part (or isn't
// a readable .docx). A store that isn't a JSON object or array, or has an
// invalid name, is left out with a warning.
DocumentData::Stores read(const QString &path);

} // namespace DocxData
