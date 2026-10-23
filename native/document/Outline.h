#pragma once

// Outline: a document's structure from its headings, for author tooling.
// Chapters are Heading 1, scenes Heading 2 (and so on): one file, its
// chapters what the navigator lists, with word counts, and moves.
//
// A section is a heading and everything after it up to the next heading of
// the same or a higher level: a chapter includes its scenes.

#include <QList>
#include <QString>
#include <QTextCharFormat>

class QTextDocument;

namespace Outline {

struct Section
{
    QString title;
    int level = 1;
    int position = 0; // the heading block's
    int words = 0; // in the section, its heading and subsections included
};

QList<Section> sections(const QTextDocument *doc);
// Words in the whole document: runs of letters or digits (apostrophes and
// hyphens inside a word don't split it); objects don't count.
int wordCount(const QTextDocument *doc);
int wordCount(const QString &text);

// Moves section `from` (with its subsections) before section `to`, or after
// the last section if `to` is the number of sections, as one undo step.
// False, changing nothing, if that is no move (onto itself or into itself).
bool moveSection(QTextDocument *doc, int from, int to);

// How a heading of `level` looks (bold, larger), as Qt's HTML headings do;
// level 0, body text, sets neither.
QTextCharFormat headingLook(int level);

} // namespace Outline
