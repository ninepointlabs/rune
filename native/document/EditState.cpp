#include "EditState.h"

#include <QTextDocument>

#include <algorithm>

QTextCursor EditState::selectionCursor() const
{
    const int last = std::max(0, target->characterCount() - 1);
    QTextCursor cursor(target);
    if (selectionStart != selectionEnd) {
        const int anchor = cursorPosition == selectionStart ? selectionEnd : selectionStart;
        cursor.setPosition(std::clamp(anchor, 0, last));
        cursor.setPosition(std::clamp(cursorPosition, 0, last), QTextCursor::KeepAnchor);
    } else {
        cursor.setPosition(std::clamp(cursorPosition, 0, last));
    }
    return cursor;
}

QList<QTextBlock> EditState::selectedBlocks() const
{
    const QTextCursor cursor = selectionCursor();
    QList<QTextBlock> blocks;
    const QTextBlock last = target->findBlock(cursor.selectionEnd());
    for (QTextBlock block = target->findBlock(cursor.selectionStart()); block.isValid(); block = block.next()) {
        blocks.append(block);
        if (block == last)
            break;
    }
    return blocks;
}
