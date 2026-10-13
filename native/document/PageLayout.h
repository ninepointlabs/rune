#pragma once

// PageLayout: a document's paper size and margins, in millimetres, and how
// they are stored in ODF.
//
// In an .odt the layout lives in styles.xml: a <style:page-layout> in its
// automatic styles, used by the master page. Qt's ODF writer produces no
// styles.xml at all, so withOdfStyles() adds one (and its manifest
// entry) to what the writer wrote. fromOdfStyles() reads it back from the
// master page named "Standard", or the first master page if none is.
//
// A header sits `headerMm` from the top edge, inside the top margin; the
// body starts at the top margin. ODF (and LibreOffice) measure it the other
// way: the page's fo:margin-top is the header's distance, and the header's
// fo:min-height (spacing included) reaches down to where the body starts.
// The footer mirrors this at the bottom. odfStyles() writes it that way when
// there is a header or footer, and fromOdfStyles() reads it back.
//
// Not stored: a different first page or left pages, more than one page
// style.

#include <QByteArray>
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

    // The page layout in an ODF styles.xml, if it has one.
    static std::optional<PageLayout> fromOdfStyles(const QByteArray &stylesXml);
    // ... in the .odt at `path` (none if it has no styles.xml or isn't one).
    static std::optional<PageLayout> fromOdtFile(const QString &path);

    // What a styles.xml holds besides the page layout: the header and
    // footer content (ODF block elements, "" for none) and the automatic
    // styles they use.
    struct OdfParts
    {
        QString header;
        QString footer;
        QString automaticStyles;
    };
    // A complete styles.xml holding this page layout and `parts`.
    QByteArray odfStyles(const OdfParts &parts = {}) const;
};

// `odt` (a whole .odt file, as Qt's writer produced it) with `stylesXml` as
// its styles.xml, listed in the manifest; replaces any styles.xml it had.
// Empty if `odt` can't be read as a zip.
QByteArray withOdfStyles(const QByteArray &odt, const QByteArray &stylesXml);
