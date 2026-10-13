#pragma once

// StoryNotes: an author's notes on the work: its characters, places and
// research, in the "notes" data store (DocumentData), saved in the file
// with the text. Each note is {"id", "kind", "name", "text"}, kind one of
// kinds(). People edit them in the Notes panel; agents read them as JSON and
// send back changes (merge()).

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace StoryNotes {

inline const QString kStore = QStringLiteral("notes");

// "character", "place", "research".
QStringList kinds();

// A new note's id: "n1", "n2", ... unused in `notes`.
QString freshId(const QJsonArray &notes);
qsizetype indexOf(const QJsonArray &notes, const QString &id);

// Merges `incoming` (an agent's): a note with the id of an existing one
// updates it (fields it leaves out are kept); one without, or with an
// unknown id, matching an existing note's kind and name (ignoring case)
// updates that; any other is added, with a fresh id. A note with
// "deleted": true and a known id is removed. Kinds outside kinds() become
// "research". Returns how many notes changed.
int merge(QJsonArray &notes, const QJsonArray &incoming);

} // namespace StoryNotes
