#pragma once

// InlineObjects: things that sit in the text as one object character
// (QChar::ObjectReplacementCharacter) whose format says what it is. The page
// number and page count fields now; citations and note anchors later.
//
// Each kind is registered once, in kinds(), with everything Rune does with
// it, so adding one doesn't mean editing the readers, writers and editor:
//  - objectType: its QTextFormat object type (allocated in FormatIds);
//  - allowedIn: the kinds of part it may appear in (DocumentParts);
//  - install: registers its drawing (a QTextObjectInterface) on a document
//    of a part it is allowed in;
//  - toOdf: its ODF, for the writers;
//  - odfElements / fromOdf: the ODF elements it is read from, and the
//    format an object character read from one gets;
//  - odfRangePrefix / fromOdfRange: or, for a kind saved as text between
//    two bookmarks (so other programs show the text and keep the marks),
//    the bookmark names' prefix and the format for that range's text;
//  - toMarkdown: what the AI sees for it (citations: Pandoc's `[@key]`).

#include "DocumentData.h"
#include "DocumentParts.h"

#include <QList>
#include <QString>
#include <QTextCharFormat>

class QTextDocument;

namespace InlineObjects {

struct OdfElement
{
    QString namespaceUri;
    QString name;
};

// What a kind's ODF writer gets besides the object's format: the document
// the object is in, and the data stores being saved with it, which it may
// add to (a citation records its sources there).
struct OdfContext
{
    const QTextDocument *doc = nullptr;
    DocumentData::Stores *data = nullptr;
};

struct Kind
{
    int objectType;
    QList<DocumentParts::Kind> allowedIn;
    void (*install)(QTextDocument *doc);
    // The ODF for one object character of this kind with `format`.
    QString (*toOdf)(const QTextFormat &format, OdfContext &context);
    QList<OdfElement> odfElements;
    // The format for an object character read from element `name` (one of
    // odfElements), formatted like the text around it (`base`).
    QTextCharFormat (*fromOdf)(const QString &name, const QTextCharFormat &base) = nullptr;
    QString odfRangePrefix;
    // The format for an object character read from the text `text` between
    // bookmarks named `name` (starting with odfRangePrefix).
    QTextCharFormat (*fromOdfRange)(const QString &name, const QString &text, const QTextCharFormat &base) = nullptr;
    QString (*toMarkdown)(const QTextFormat &format) = nullptr;
    // The format properties it sets besides the object type, which text
    // typed next to one must not take on.
    QList<int> formatProperties;
};

// Every kind.
const QList<Kind> &kinds();

// The kind of an object character with `format`, or nullptr (not an inline
// object Rune knows, e.g. an image).
const Kind *forFormat(const QTextFormat &format);
// The kind read from ODF element {namespaceUri}name in a part of kind
// `part`, or nullptr (the reader then reads the element as text).
const Kind *forOdfElement(QStringView namespaceUri, QStringView name, DocumentParts::Kind part);

// The kind saved between bookmarks named `name` in a part of kind `part`,
// or nullptr.
const Kind *forOdfRange(QStringView name, DocumentParts::Kind part);

// `format` without any inline object's type and properties: for text typed
// next to an object, which would otherwise take on its format.
QTextCharFormat withoutObject(QTextCharFormat format);

// Registers the drawing of every kind allowed in `part` on `doc`'s current
// layout (again after anything replaces the layout).
void installAll(QTextDocument *doc, DocumentParts::Kind part);

} // namespace InlineObjects
