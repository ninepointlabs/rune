#pragma once

// Sources: the document's sources list, CSL-JSON items in the "sources"
// data store (DocumentData), and what people and agents do with them.
//
// Every source has a unique "id", its citation key ("smith2020"): what
// citations point at and what Markdown cites as [@smith2020]. A source an
// agent added is "unreviewed" (CSL-JSON's custom object:
// {"custom": {"unreviewed": true}}) until a person confirms it: models
// invent plausible-looking references. A checked link records whether its
// DOI or URL resolved ("custom": {"link": "ok" | "broken"}).

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace Sources {

inline const QString kStore = QStringLiteral("sources");

// `item` ready to store: a type ("book" if none) and an id, its own if it
// has a free one, else one made from the first author and year ("smith2020",
// then "smith2020a", ...), unique among `existing` (besides `item` itself,
// by `replacing`).
QJsonObject normalized(QJsonObject item, const QJsonArray &existing, const QString &replacing = {});
// The index of the source with `id`, or -1.
qsizetype indexOf(const QJsonArray &sources, const QString &id);
QJsonObject find(const QJsonArray &sources, const QString &id);

// For lists: "Smith & Doe 2020 · Title".
QString label(const QJsonObject &item);

bool isUnreviewed(const QJsonObject &item);
QJsonObject withUnreviewed(QJsonObject item, bool unreviewed);
// "ok", "broken" or "" (not checked).
QString linkState(const QJsonObject &item);
QJsonObject withLinkState(QJsonObject item, const QString &state);

// Merges `incoming` (an agent's or an import's sources) into `sources`:
// one with the id of an existing source replaces it, the rest are added
// with fresh ids where theirs are taken. With `unreviewed`, every source
// merged is marked unreviewed. Returns the ids merged, in order.
QStringList merge(QJsonArray &sources, const QJsonArray &incoming, bool unreviewed);

// Sources from BibTeX (@book{key, author = {...}, ...}): the common entry
// types and fields, braces and quotes, "and"-separated names in either
// order. Entries it can't read are skipped.
QJsonArray fromBibTeX(const QString &bibtex);

} // namespace Sources
