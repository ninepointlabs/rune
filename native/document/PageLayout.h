#pragma once

// PageLayout: a document's paper size and margins, in millimetres, kept in
// the body's root frame format (storeIn()). Reading and writing it in ODF
// is OdfStyles'.

#include <QLocale>
#include <QMarginsF>
#include <QSizeF>
#include <QVariantMap>

#include <optional>

class QTextDocument;

struct PageLayout
{
    qreal widthMm = 215.9;
    qreal heightMm = 279.4;
    qreal topMm = 25.4;
    qreal bottomMm = 25.4;
    qreal leftMm = 25.4;
    qreal rightMm = 25.4;
    // Distance of the header's top from the page's top edge, and of the
    // footer's bottom from the bottom edge, as ODF stores them; at most the
    // margin. (Rune centres them in their margins and saves where that puts
    // them: see HeaderFooter.)
    qreal headerMm = 12.7;
    qreal footerMm = 12.7;

    // Letter in the countries that use it, A4 elsewhere; 1 inch (2.54 cm)
    // margins either way.
    static PageLayout defaultFor(QLocale::Territory territory);
    // Whether `territory` uses Letter paper (and so inches in Page setup).
    static bool usesLetter(QLocale::Territory territory);

    // {width, height, top, bottom, left, right, header, footer} in
    // millimetres, for QML.
    QVariantMap toMap() const;
    // Missing or non-positive sizes keep this layout's values; margins are
    // kept at least 0 and leave at least 20 mm of text area each way; the
    // header and footer distances are kept between 0 and their margin.
    static PageLayout fromMap(const QVariantMap &map, const PageLayout &fallback);

    // At 96 pixels per inch, the scale the editor lays text out at.
    QSizeF sizePx() const;
    QMarginsF marginsPx() const;

    // The layout stored in `doc`'s root frame format (see storeIn()), if
    // it has one.
    static std::optional<PageLayout> fromDocument(const QTextDocument *doc);
    // Stores this layout in `doc`'s root frame format, twice: its margins
    // in pixels, which the text layout uses, and the exact millimetre
    // values as user properties, which fromDocument() reads. An ordinary
    // edit: undo and redo restore both.
    void storeIn(QTextDocument *doc) const;

    bool operator==(const PageLayout &other) const;
    bool operator!=(const PageLayout &other) const { return !(*this == other); }
};

