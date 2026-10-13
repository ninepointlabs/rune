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
// toOdf() writes such a document as the ODF of a <style:header> or
// <style:footer>; OdfReader::readPagePart() reads one back.
//
// Written: paragraphs with their alignment; bold, italic, underline, font
// family, size and color; tabs, runs of spaces and line breaks; the two
// fields. Not written: lists and tables (their paragraphs are written as
// plain paragraphs), images.

#include <QColor>
#include <QObject>
#include <QSizeF>
#include <QString>
#include <QTextCharFormat>
#include <QTextObjectInterface>

class QTextDocument;

namespace PageParts {

constexpr int kFieldObject = QTextFormat::UserObject + 1;
// Which field an object character is (a Field).
constexpr int kFieldKind = QTextFormat::UserProperty + 120;

enum class Field { PageNumber, PageCount };

// `base` (the formatting around it) as the format of a `field` character.
QTextCharFormat fieldFormat(const QTextCharFormat &base, Field field);
bool isField(const QTextFormat &format);

// The ODF block elements for `doc`, its automatic styles added to
// `automaticStyles` under names starting with `stylePrefix`. Every
// paragraph gets the header and footer tab stops: centre at half
// `textWidthMm`, right at `textWidthMm`, and `doc`'s default font family
// and size, which its unformatted text is in. "" for an empty document.
QString toOdf(const QTextDocument *doc, const QString &stylePrefix, qreal textWidthMm, QString &automaticStyles);

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

    QSizeF intrinsicSize(QTextDocument *doc, int position, const QTextFormat &format) override;
    void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *doc, int position,
                    const QTextFormat &format) override;

private:
    QFont fontFor(QTextDocument *doc, const QTextFormat &format) const;
};
