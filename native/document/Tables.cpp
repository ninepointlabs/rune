#include "Tables.h"

#include <QTextBlock>
#include <QTextDocument>
#include <QTextTable>

#include <algorithm>

namespace Tables {

namespace {

// The cell after (or before) `cell` in reading order, visiting a spanned
// cell once, at its top-left; invalid past either end of the table.
QTextTableCell nextCell(QTextTable *table, const QTextTableCell &cell, bool forward)
{
    int row = cell.row(), column = cell.column();
    for (;;) {
        column += forward ? 1 : -1;
        if (column >= table->columns()) {
            column = 0;
            ++row;
        } else if (column < 0) {
            column = table->columns() - 1;
            --row;
        }
        if (row < 0 || row >= table->rows())
            return {};
        const QTextTableCell candidate = table->cellAt(row, column);
        if (candidate.row() == row && candidate.column() == column)
            return candidate;
    }
}

} // namespace

QTextTableFormat format()
{
    QTextTableFormat format;
    format.setBorder(1);
    format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    format.setBorderCollapse(true);
    format.setCellSpacing(0);
    format.setCellPadding(4);
    format.setWidth(QTextLength(QTextLength::PercentageLength, 100));
    return format;
}

int insert(QTextCursor cursor, int rows, int columns)
{
    cursor.clearSelection();
    QTextTable *table = cursor.insertTable(std::clamp(rows, 1, kMaxSize), std::clamp(columns, 1, kMaxSize), format());
    return table ? table->cellAt(0, 0).firstCursorPosition().position() : -1;
}

int edit(const QTextCursor &cursor, bool rows, bool insert, bool before)
{
    QTextTable *table = cursor.currentTable();
    if (!table)
        return -1;
    const QTextTableCell cell = table->cellAt(cursor);
    if (!cell.isValid() || (!insert && (rows ? table->rows() : table->columns()) <= 1))
        return -1;
    const int row = cell.row(), column = cell.column();
    if (insert && rows)
        table->insertRows(before ? row : row + cell.rowSpan(), 1);
    else if (insert)
        table->insertColumns(before ? column : column + cell.columnSpan(), 1);
    else if (rows)
        table->removeRows(row, 1);
    else
        table->removeColumns(column, 1);
    if (insert)
        return -1;
    // The cursor's cell is gone: to the one that took its place.
    const QTextTableCell landing = table->cellAt(std::min(row, table->rows() - 1),
                                                 std::min(column, table->columns() - 1));
    return landing.firstCursorPosition().position();
}

int remove(const QTextCursor &at)
{
    QTextTable *table = at.currentTable();
    if (!table)
        return -1;
    QTextDocument *doc = at.document();
    // The table's frame runs from one position before its first cell to one
    // after its last, and separates the paragraphs either side of it: with
    // it gone they'd join into one. So in the same undo step, the paragraph
    // after the table is split off again with its own formats.
    const int position = table->firstPosition() - 1;
    const QTextBlock after = doc->findBlock(table->lastPosition() + 2);
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    cursor.setPosition(position);
    cursor.setPosition(table->lastPosition() + 1, QTextCursor::KeepAnchor);
    const QTextBlockFormat afterBlock = after.blockFormat();
    const QTextCharFormat afterChar = after.charFormat();
    cursor.removeSelectedText();
    cursor.insertBlock(afterBlock, afterChar);
    cursor.endEditBlock();
    return std::clamp(position, 0, doc->characterCount() - 1);
}

int adjacentCell(const QTextCursor &cursor, bool forward)
{
    QTextTable *table = cursor.currentTable();
    if (!table)
        return -1;
    QTextTableCell cell = nextCell(table, table->cellAt(cursor), forward);
    if (!cell.isValid() && forward) {
        table->appendRows(1);
        cell = table->cellAt(table->rows() - 1, 0);
    }
    return cell.isValid() ? cell.firstCursorPosition().position() : -1;
}

} // namespace Tables
