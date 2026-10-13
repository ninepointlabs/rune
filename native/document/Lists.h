#pragma once

// Lists: bullet and numbered lists as Rune builds them, shared by editing
// (DocumentController) and reading (OdfReader) so an opened document has the
// structure the UI would have built.
//
// Qt does not join lists: QTextCursor::createList() always makes a new
// QTextList, even right next to an identical one, and each QTextList numbers
// itself from 1 and is written to ODF as its own <text:list>. So these
// functions reuse an adjacent compatible list where one exists and only
// create a list when there is none. Nesting is QTextListFormat::indent():
// one QTextList per run of siblings at a level. Nested bullet levels use
// disc/circle/square, all of which count as "bullet"; numbered lists are
// decimal at every level.

#include <QList>
#include <QTextBlock>
#include <QTextListFormat>

class QTextList;

namespace Lists {

enum class Kind { None, Bullet, Numbered };

Kind kind(const QTextList *list);
// The QTextListFormat style for a list of `kind` at nesting `indent` (1-based).
QTextListFormat::Style style(Kind kind, int indent);

// Takes `block` out of its list as a plain paragraph. QTextList::remove()
// would instead add the list's indent to the block's own, leaving it
// indented as if it were still a list item.
void removeFromList(const QTextBlock &block);

// Makes `blocks` (consecutive, in document order) a top-level list of
// `kind`, joining one directly before or after them (and folding the two
// together where they bridge both). One undo step.
void add(const QList<QTextBlock> &blocks, Kind kind);
// Takes every list item in `blocks` out of its list. One undo step.
void remove(const QList<QTextBlock> &blocks);
// Moves each list item in `blocks` `delta` levels deeper (+1) or shallower
// (-1); promoting a top-level item does nothing. One undo step.
void changeLevel(const QList<QTextBlock> &blocks, int delta);

} // namespace Lists
