#pragma once

// Tables: the table format Rune builds (shared with OdfReader so an opened
// document's tables look the ones the UI makes) and the edits on them.
//
// QTextTable has no "next cell" operation and the TextEdit owns the visible
// cursor, so the edits compute where the cursor should go and return it, for
// DocumentController to ask the TextEdit to move there. -1: it stays.

#include <QTextCursor>
#include <QTextTableFormat>

namespace Tables {

// Rows and columns insert() makes at most (the LibreOfficeKit engine's limit).
constexpr int kMaxSize = 50;

QTextTableFormat format();

// A rows × columns table at `cursor` (clamped to 1..kMaxSize), returning its
// first cell's position; -1 if none was made.
int insert(QTextCursor cursor, int rows, int columns);

// In the table at `cursor`, inserts a row or column (`rows`) after its
// cell, or before with `before`; or (`insert` false) deletes the cell's row
// or column unless it is the last one: QTextTable::removeRows() and
// removeColumns() delete the whole table when asked to remove every row or
// column. Returns where the cursor goes when its cell is gone.
int edit(const QTextCursor &cursor, bool rows, bool insert, bool before = false);

// The table at `cursor`, contents and all, as one undo step; returns where
// the table was.
int remove(const QTextCursor &cursor);

// The cell after (`forward`) or before the cursor's, in reading order.
// Forward from the last cell appends a row; back from the first is -1.
int adjacentCell(const QTextCursor &cursor, bool forward);

} // namespace Tables
