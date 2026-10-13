#pragma once

// OdfStyles: a document's styles.xml, which holds its page layout, header
// and footer (Qt has no ODF reader, and Rune writes the file itself).
//
// The layout is a <style:page-layout> in styles.xml's automatic styles,
// used by the master page. readPageLayout() reads it back from the master
// page named "Standard", or the first master page if none is.
//
// A header sits `headerMm` from the top edge, inside the top margin; the
// body starts at the top margin. ODF (and LibreOffice) measure it the other
// way: the page's fo:margin-top is the header's distance, and the header's
// fo:min-height (spacing included) reaches down to where the body starts.
// The footer mirrors this at the bottom. write() writes it that way when
// there is a header or footer, and readPageLayout() reads it back.
//
// Not stored: a different first page or left pages, more than one page
// style.

#include "PageLayout.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace OdfStyles {

// The page layout in an ODF styles.xml, if it has one.
std::optional<PageLayout> readPageLayout(const QByteArray &stylesXml);
// ... in the .odt at `path` (none if it has no styles.xml or isn't one).
std::optional<PageLayout> readPageLayoutFile(const QString &path);

// What a styles.xml holds besides the page layout: the header and footer
// content (ODF block elements, "" for none; see OdfTextWriter) and the
// automatic styles they use.
struct Parts
{
    QString header;
    QString footer;
    QString automaticStyles;
};
// A complete styles.xml holding `layout` and `parts`.
QByteArray write(const PageLayout &layout, const Parts &parts = {});

} // namespace OdfStyles
