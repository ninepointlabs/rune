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
//  - toOdf: its ODF element, for the writers;
//  - odfElements / fromOdf: the ODF elements it is read from, and the
//    format an object character read from one gets.
// Not yet: its Markdown for the AI's context and replies. That arrives with
// the first kind that appears in the body (citations: Pandoc's `[@key]`;
// see docs/architecture-plan.md).

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

struct Kind
{
    int objectType;
    QList<DocumentParts::Kind> allowedIn;
    void (*install)(QTextDocument *doc);
    // The ODF for one object character of this kind with `format`.
    QString (*toOdf)(const QTextFormat &format);
    QList<OdfElement> odfElements;
    // The format for an object character read from element `name` (one of
    // odfElements), formatted like the text around it (`base`).
    QTextCharFormat (*fromOdf)(const QString &name, const QTextCharFormat &base);
};

// Every kind.
const QList<Kind> &kinds();

// The kind of an object character with `format`, or nullptr (not an inline
// object Rune knows, e.g. an image).
const Kind *forFormat(const QTextFormat &format);
// The kind read from ODF element {namespaceUri}name in a part of kind
// `part`, or nullptr (the reader then reads the element as text).
const Kind *forOdfElement(QStringView namespaceUri, QStringView name, DocumentParts::Kind part);

// Registers the drawing of every kind allowed in `part` on `doc`'s current
// layout (again after anything replaces the layout).
void installAll(QTextDocument *doc, DocumentParts::Kind part);

} // namespace InlineObjects
