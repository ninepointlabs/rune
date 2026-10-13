#pragma once

// PageParts: what headers and footers are made of beyond plain text.
//
// A header or footer is its own QTextDocument (see HeaderFooter). A
// page number or page count in it is a field: one object character
// (QChar::ObjectReplacementCharacter) whose format has object type
// kFieldObject and says which field it is. PageFieldHandler draws it, as
// the number for whichever page is being drawn; it must be registered on
// the document's layout (install()).
//
// OdfWriter writes a header or footer (OdfTextWriter, the fields through
// their InlineObjects kind) and OdfReader::readPagePart() reads one back.

#include "FormatIds.h"
#include "InlineObjects.h"

#include <QColor>
#include <QObject>
#include <QSizeF>
#include <QString>
#include <QTextCharFormat>
#include <QTextObjectInterface>

class QTextDocument;

namespace PageParts {

constexpr int kFieldObject = FormatIds::kPageFieldObject;
// Which field an object character is (a Field).
constexpr int kFieldKind = FormatIds::kPageFieldKind;

enum class Field { PageNumber, PageCount };

// `base` (the formatting around it) as the format of a `field` character.
QTextCharFormat fieldFormat(const QTextCharFormat &base, Field field);
bool isField(const QTextFormat &format);
// The fields as an inline object kind (see InlineObjects): in headers and
// footers, written and read as <text:page-number> / <text:page-count>.
InlineObjects::Kind fieldKind();

// Whether `doc` has nothing in it but empty paragraphs.
bool isEmpty(const QTextDocument *doc);

} // namespace PageParts

// Draws page number and page count fields: the numbers for `page` of
// `pageCount`. Each field takes the room of the widest page number, so the
// layout doesn't change from page to page; narrower numbers are centred in
// it. Fields draw in their format's color, else `color`.
class PageFieldHandler : public QObject, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    using QObject::QObject;

    // Registers a handler on `doc`'s current layout (again after anything
    // replaces the layout) and returns it; `doc` owns it.
    static PageFieldHandler *install(QTextDocument *doc);

    int page = 1;
    int pageCount = 1;
    QColor color = Qt::black;
    // Draw nothing, keeping the room: what the header and footer TextEdits
    // get (see HeaderFooter::setEditing()).
    bool blank = false;

    QSizeF intrinsicSize(QTextDocument *doc, int position, const QTextFormat &format) override;
    void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *doc, int position,
                    const QTextFormat &format) override;

private:
    QFont fontFor(QTextDocument *doc, const QTextFormat &format) const;
};
