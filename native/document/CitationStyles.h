#pragma once

// CitationStyles: in-text citations and bibliography entries for sources in
// CSL-JSON (the format Zotero, Mendeley and Pandoc exchange), in four
// built-in styles: APA 7, MLA 9, Chicago author-date (17th) and IEEE.
//
// Implemented here rather than with a CSL engine (citeproc-js would mean
// running a JavaScript engine for this). Covered: authors (persons and
// organisations), year, title, container (journal, book, site), publisher
// and place, edition, volume, issue, pages, URL and DOI, for the common
// types: book, chapter, journal/magazine/newspaper article, web page,
// report and thesis; anything else is written as a book or article by
// whether it has a container. A locator ("12", "12-14", "p. 12") is a page
// reference.

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace CitationStyles {

enum class Style { Apa, Mla, Chicago, Ieee };

// "apa", "mla", "chicago", "ieee", and back (APA for anything else).
QString id(Style style);
Style fromId(const QString &id);
QStringList ids();
// "APA 7", "MLA 9", "Chicago (author-date)", "IEEE".
QString displayName(Style style);

struct Run
{
    QString text;
    bool italic = false;
};
using Runs = QList<Run>;
QString plain(const Runs &runs);

// The in-text citation of `items`, in the order cited, with `locator`
// (page) for the last. `numbers`: each item's number (IEEE).
QString inText(Style style, const QList<QJsonObject> &items, const QString &locator, const QList<int> &numbers);
// One bibliography entry; `number` for IEEE's "[n]".
Runs entry(Style style, const QJsonObject &item, int number);
// The bibliography's heading: "References" or "Works Cited".
QString bibliographyTitle(Style style);
// Whether the bibliography lists sources in the order first cited (IEEE),
// rather than alphabetically.
bool ordersByCitation(Style style);
// Alphabetical order: first author's family name, else title; then year.
QString sortKey(const QJsonObject &item);

// The year (first of "issued"), or "" if none.
QString year(const QJsonObject &item);

} // namespace CitationStyles
