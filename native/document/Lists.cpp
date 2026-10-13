#include "Lists.h"

#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>

#include <algorithm>

namespace Lists {

namespace {

int listIndent(const QTextBlock &block)
{
    return block.textList() ? block.textList()->format().indent() : 0;
}

} // namespace

Kind kind(const QTextList *list)
{
    if (!list)
        return Kind::None;
    switch (list->format().style()) {
    case QTextListFormat::ListDisc:
    case QTextListFormat::ListCircle:
    case QTextListFormat::ListSquare:
        return Kind::Bullet;
    default:
        return Kind::Numbered;
    }
}

QTextListFormat::Style style(Kind kind, int indent)
{
    if (kind == Kind::Numbered)
        return QTextListFormat::ListDecimal;
    static const QTextListFormat::Style bullets[] = {
        QTextListFormat::ListDisc, QTextListFormat::ListCircle, QTextListFormat::ListSquare};
    return bullets[std::max(0, indent - 1) % 3];
}

void removeFromList(const QTextBlock &block)
{
    QTextBlockFormat format = block.blockFormat();
    format.setObjectIndex(-1);
    QTextCursor(block).setBlockFormat(format);
}

void add(const QList<QTextBlock> &blocks, Kind listKind)
{
    if (blocks.isEmpty())
        return;
    QTextCursor edit(blocks.constFirst());
    edit.beginEditBlock();

    // Join a top-level list of this kind directly before or after the
    // blocks; failing that, start one.
    const auto joinable = [listKind](const QTextBlock &block) {
        return block.isValid() && kind(block.textList()) == listKind && listIndent(block) == 1
            ? block.textList() : nullptr;
    };
    QTextList *before = joinable(blocks.constFirst().previous());
    QTextList *after = joinable(blocks.constLast().next());
    QTextList *target = before ? before : after;
    if (!target) {
        QTextListFormat format;
        format.setStyle(style(listKind, 1));
        target = QTextCursor(blocks.constFirst()).createList(format);
    }
    for (const QTextBlock &block : blocks)
        if (block.textList() != target)
            target->add(block);
    // Bridging two lists: fold the second into the first.
    if (before && after && before != after) {
        QList<QTextBlock> items;
        for (int i = 0; i < after->count(); ++i)
            items.append(after->item(i));
        for (const QTextBlock &item : std::as_const(items))
            target->add(item);
    }

    edit.endEditBlock();
}

void remove(const QList<QTextBlock> &blocks)
{
    if (blocks.isEmpty())
        return;
    QTextCursor edit(blocks.constFirst());
    edit.beginEditBlock();
    for (const QTextBlock &block : blocks)
        if (block.textList())
            removeFromList(block);
    edit.endEditBlock();
}

void changeLevel(const QList<QTextBlock> &blocks, int delta)
{
    if (blocks.isEmpty())
        return;
    QTextCursor edit(blocks.constFirst());
    edit.beginEditBlock();

    for (const QTextBlock &block : blocks) {
        QTextList *list = block.textList();
        if (!list)
            continue;
        const Kind listKind = kind(list);
        const int target = list->format().indent() + delta;
        if (target < 1)
            continue;

        // The block's new siblings: the nearest earlier item at the target
        // level, skipping deeper items, before reaching a shallower one
        // (the parent); else an item right after it at that level.
        QTextList *dest = nullptr;
        for (QTextBlock previous = block.previous(); previous.isValid() && previous.textList();
             previous = previous.previous()) {
            const int indent = listIndent(previous);
            if (indent < target)
                break;
            if (indent == target) {
                if (kind(previous.textList()) == listKind)
                    dest = previous.textList();
                break;
            }
        }
        const QTextBlock next = block.next();
        if (!dest && listIndent(next) == target && kind(next.textList()) == listKind)
            dest = next.textList();

        if (dest) {
            dest->add(block);
        } else {
            QTextListFormat format = list->format();
            format.setIndent(target);
            format.setStyle(style(listKind, target));
            QTextCursor(block).createList(format);
        }

        // Promoting: the old list's following items now nest under this
        // block, so they become a list of their own and number from 1.
        if (delta < 0) {
            QTextList *children = nullptr;
            for (QTextBlock after = block.next(); after.isValid() && listIndent(after) > target;
                 after = after.next()) {
                if (after.textList() != list)
                    continue;
                if (children)
                    children->add(after);
                else
                    children = QTextCursor(after).createList(list->format());
            }
        }
    }

    edit.endEditBlock();
}

} // namespace Lists
