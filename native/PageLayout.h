#pragma once

// PageLayout: a document's paper size and margins, in millimetres, and how
// they are stored in ODF.
//
// In an .odt the layout lives in styles.xml: a <style:page-layout> in its
// automatic styles, used by the master page. Qt's ODF writer produces no
// styles.xml at all, so withOdfPageLayout() adds one (and its manifest
// entry) to what the writer wrote. fromOdfStyles() reads it back from the
// master page named "Standard", or the first master page if none is.
//
// Not stored: header and footer content, page numbering, more than one
// page style.

#include <QByteArray>
#include <QLocale>
#include <QMarginsF>
#include <QSizeF>
#include <QVariantMap>

#include <optional>

struct PageLayout
{
    qreal widthMm = 215.9;
    qreal heightMm = 279.4;
    qreal topMm = 25.4;
    qreal bottomMm = 25.4;
    qreal leftMm = 25.4;
    qreal rightMm = 25.4;

    // Letter in the countries that use it, A4 elsewhere; 1 inch (2.54 cm)
    // margins either way.
    static PageLayout defaultFor(QLocale::Territory territory);
    // Whether `territory` uses Letter paper (and so inches in Page setup).
    static bool usesLetter(QLocale::Territory territory);

    // {width, height, top, bottom, left, right} in millimetres, for QML.
    QVariantMap toMap() const;
    // Missing or non-positive sizes keep this layout's values; margins are
    // kept at least 0 and leave at least 20 mm of text area each way.
    static PageLayout fromMap(const QVariantMap &map, const PageLayout &fallback);

    // At 96 pixels per inch, the scale the editor lays text out at.
    QSizeF sizePx() const;
    QMarginsF marginsPx() const;

    bool operator==(const PageLayout &other) const;
    bool operator!=(const PageLayout &other) const { return !(*this == other); }

    // The page layout in an ODF styles.xml, if it has one.
    static std::optional<PageLayout> fromOdfStyles(const QByteArray &stylesXml);
    // ... in the .odt at `path` (none if it has no styles.xml or isn't one).
    static std::optional<PageLayout> fromOdtFile(const QString &path);
    // A complete styles.xml holding just this page layout.
    QByteArray odfStyles() const;
};

// `odt` (a whole .odt file, as Qt's writer produced it) with `layout`'s
// styles.xml added and listed in the manifest; replaces any styles.xml it
// had. Empty if `odt` can't be read as a zip.
QByteArray withOdfPageLayout(const QByteArray &odt, const PageLayout &layout);
