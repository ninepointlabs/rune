#include "DocumentController.h"

#include <QBuffer>
#include <QDebug>
#include <QFileInfo>
#include <QList>
#include <QScopedValueRollback>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextDocumentWriter>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

#include <algorithm>
#include <memory>

DocumentController::DocumentController(QObject *parent)
    : QObject(parent), m_document(new QTextDocument(this))
{
    connect(m_document, &QTextDocument::modificationChanged, this, &DocumentController::dirtyChanged);
    connect(m_document, &QTextDocument::contentsChange, this, &DocumentController::onContentsChange);
}

DocumentController::~DocumentController()
{
    // The TextEdit may outlive us during QML teardown and still points at
    // m_document; hand the document to its wrapper so they die together.
    if (m_qmlDocument)
        m_document->setParent(m_qmlDocument);
}

void DocumentController::setQmlDocument(QQuickTextDocument *qmlDocument)
{
    if (m_qmlDocument == qmlDocument)
        return;
    m_qmlDocument = qmlDocument;
    if (m_qmlDocument)
        m_qmlDocument->setTextDocument(m_document);
    emit documentChanged();
    emit formatChanged();
}

void DocumentController::newDocument()
{
    m_document->clear();
    m_document->setModified(false);
    clearPending();
    if (!m_currentPath.isEmpty()) {
        m_currentPath.clear();
        emit currentPathChanged();
    }
    emit formatChanged();
}

bool DocumentController::saveToOdf(const QString &path)
{
    // Sanitize a copy: the export workarounds must not rewrite what the
    // user is editing.
    std::unique_ptr<QTextDocument> copy(m_document->clone());
    sanitizeForOdfExport(copy.get());

    // The ODF writer close()s its device, which QSaveFile treats as fatal, so
    // render into memory and commit the bytes atomically.
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QTextDocumentWriter writer(&buffer, QByteArrayLiteral("ODF"));
    if (!writer.write(copy.get())) {
        qWarning() << "DocumentController: ODF serialisation failed";
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(buffer.data()) != buffer.data().size()
        || !file.commit()) {
        qWarning() << "DocumentController: cannot write" << path << file.errorString();
        return false;
    }

    m_document->setModified(false);
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (m_currentPath != absolute) {
        m_currentPath = absolute;
        emit currentPathChanged();
    }
    return true;
}

// --- ODF export sanitizing -------------------------------------------------

namespace {

// Rule 1: list directly followed by a table.
//
// Qt's ODF writer (QTextDocumentWriter, format "ODF") emits malformed XML
// when a table immediately follows a list block with no plain paragraph in
// between, and LibreOffice then silently drops the whole table. Found and
// verified in native-spike/ (XML inspection plus a real soffice round-trip
// showing the data loss); the verified fix is an empty paragraph between
// the list and the table. A table made by QTextCursor::insertTable() at the
// end of a list item (insertTable() below) has the same shape as one parsed
// by setHtml(): a root-frame child right after the list block, with the
// same malformed output when unsanitized.
//
// Collects the end-of-text position of every list block that is directly
// followed by a table, walking nested frames and table cells.
void collectListTableJoins(QTextFrame::iterator it, QList<int> &positions)
{
    QTextBlock previous;
    for (; !it.atEnd(); ++it) {
        QTextFrame *child = it.currentFrame();
        if (!child) {
            previous = it.currentBlock();
            continue;
        }
        auto *table = qobject_cast<QTextTable *>(child);
        if (table && previous.isValid() && previous.textList())
            positions << previous.position() + previous.length() - 1;
        if (table) {
            for (int r = 0; r < table->rows(); ++r)
                for (int c = 0; c < table->columns(); ++c) {
                    const QTextTableCell cell = table->cellAt(r, c);
                    if (cell.row() == r && cell.column() == c) // spanned cells once
                        collectListTableJoins(cell.begin(), positions);
                }
        } else {
            collectListTableJoins(child->begin(), positions);
        }
        previous = QTextBlock();
    }
}

} // namespace

void DocumentController::sanitizeForOdfExport(QTextDocument *doc)
{
    if (!doc)
        return;

    QList<int> joins;
    collectListTableJoins(doc->rootFrame()->begin(), joins);
    // Back to front so earlier positions stay valid.
    std::sort(joins.begin(), joins.end(), std::greater<int>());
    for (const int position : std::as_const(joins)) {
        QTextCursor cursor(doc);
        cursor.setPosition(position);
        // A default block format carries no list membership.
        cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    }
}

// --- Character formatting --------------------------------------------------

void DocumentController::setSelection(int cursorPosition, int selectionStart, int selectionEnd)
{
    m_cursorPosition = cursorPosition;
    m_selectionStart = selectionStart;
    m_selectionEnd = selectionEnd;
    if (m_pendingPosition >= 0 && (selectionStart != selectionEnd || cursorPosition != m_pendingPosition))
        clearPending();
    emit formatChanged();
}

void DocumentController::clearPending()
{
    m_pendingPosition = -1;
    m_pendingFormat = QTextCharFormat();
    m_pendingClearForeground = false;
}

bool DocumentController::hasAttr(const QTextCharFormat &f, Attr attr)
{
    switch (attr) {
    case Attr::Bold: return f.fontWeight() >= QFont::Bold;
    case Attr::Italic: return f.fontItalic();
    case Attr::Underline: return f.fontUnderline();
    }
    return false;
}

void DocumentController::setAttr(QTextCharFormat &f, Attr attr, bool on)
{
    switch (attr) {
    case Attr::Bold: f.setFontWeight(on ? QFont::Bold : QFont::Normal); break;
    case Attr::Italic: f.setFontItalic(on); break;
    case Attr::Underline: f.setFontUnderline(on); break;
    }
}

// The QML-side selection as a cursor on our document, clamped in case the
// TextEdit reported positions from before an edit.
QTextCursor DocumentController::selectionCursor() const
{
    const int last = std::max(0, m_document->characterCount() - 1);
    QTextCursor cursor(m_document);
    if (m_selectionStart != m_selectionEnd) {
        const int anchor = m_cursorPosition == m_selectionStart ? m_selectionEnd : m_selectionStart;
        cursor.setPosition(std::clamp(anchor, 0, last));
        cursor.setPosition(std::clamp(m_cursorPosition, 0, last), QTextCursor::KeepAnchor);
    } else {
        cursor.setPosition(std::clamp(m_cursorPosition, 0, last));
    }
    return cursor;
}

// With a selection: true only if every selected character has `attr`.
// Collapsed: the pending format if armed here, else the character before
// the cursor (what typing would inherit).
bool DocumentController::attrState(Attr attr) const
{
    const QTextCursor cursor = selectionCursor();
    if (!cursor.hasSelection()) {
        if (m_pendingPosition == cursor.position()) {
            const int property = attr == Attr::Bold ? QTextFormat::FontWeight
                               : attr == Attr::Italic ? QTextFormat::FontItalic
                                                      : QTextFormat::TextUnderlineStyle;
            if (m_pendingFormat.hasProperty(property))
                return hasAttr(m_pendingFormat, attr);
        }
        return hasAttr(cursor.charFormat(), attr);
    }

    const int start = cursor.selectionStart(), end = cursor.selectionEnd();
    for (QTextBlock block = m_document->findBlock(start); block.isValid() && block.position() < end;
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.position() + fragment.length() <= start || fragment.position() >= end)
                continue;
            if (!hasAttr(fragment.charFormat(), attr))
                return false;
        }
    }
    return true;
}

void DocumentController::toggle(Attr attr)
{
    QTextCharFormat change;
    setAttr(change, attr, !attrState(attr));
    applyCharFormat(change);
}

namespace {

// Removes any explicit text color from [start, end). mergeCharFormat() can't
// do this: a format with the property cleared merges as "no change", and an
// invalid QBrush is stored as a real property that the ODF writer exports
// as fo:color="#000000". So rewrite each fragment's format without it.
void clearForegroundIn(QTextDocument *doc, int start, int end)
{
    struct Range { int start, end; QTextCharFormat format; };
    QList<Range> ranges;
    for (QTextBlock block = doc->findBlock(start); block.isValid() && block.position() < end;
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const int from = std::max(start, fragment.position());
            const int to = std::min(end, fragment.position() + fragment.length());
            if (from < to && fragment.charFormat().hasProperty(QTextFormat::ForegroundBrush))
                ranges.append({from, to, fragment.charFormat()});
        }
    }
    // Collected first: setCharFormat() splits and merges fragments.
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (Range &range : ranges) {
        range.format.clearForeground();
        cursor.setPosition(range.start);
        cursor.setPosition(range.end, QTextCursor::KeepAnchor);
        cursor.setCharFormat(range.format);
    }
    cursor.endEditBlock();
}

} // namespace

// Applies `change` (or, with `clearForeground`, removes the text color) to
// the selection; with none, to the word around the cursor or, outside a
// word, to the next text typed at the cursor.
void DocumentController::applyCharFormat(const QTextCharFormat &change, bool clearForeground)
{
    QTextCursor cursor = selectionCursor();
    if (!cursor.hasSelection()) {
        // Strictly inside a word: format the whole word, as word processors do.
        const int position = cursor.position();
        QTextCursor word = cursor;
        word.select(QTextCursor::WordUnderCursor);
        if (word.selectionStart() < position && position < word.selectionEnd()) {
            cursor = word;
        } else {
            // Nothing to format yet: arm it for the next insertion here.
            m_pendingFormat.merge(change);
            if (clearForeground) {
                m_pendingFormat.clearForeground();
                m_pendingClearForeground = true;
            } else if (change.hasProperty(QTextFormat::ForegroundBrush)) {
                m_pendingClearForeground = false;
            }
            m_pendingPosition = position;
            emit formatChanged();
            return;
        }
    }
    if (clearForeground)
        clearForegroundIn(m_document, cursor.selectionStart(), cursor.selectionEnd());
    else
        cursor.mergeCharFormat(change);
    emit formatChanged();
}

// The format the queries report: the first selected character's, or what
// typing at the collapsed cursor would produce (pending format included).
QTextCharFormat DocumentController::currentCharFormat() const
{
    QTextCursor cursor = selectionCursor();
    if (cursor.hasSelection()) {
        cursor.setPosition(cursor.selectionStart() + 1);
        return cursor.charFormat();
    }
    QTextCharFormat format = cursor.charFormat();
    if (m_pendingPosition == cursor.position()) {
        format.merge(m_pendingFormat);
        if (m_pendingClearForeground)
            format.clearForeground();
    }
    return format;
}

void DocumentController::onContentsChange(int position, int removed, int added)
{
    Q_UNUSED(removed);
    if (m_selfEdit || m_pendingPosition < 0 || added <= 0 || position != m_pendingPosition)
        return;
    QTextCursor cursor(m_document);
    cursor.setPosition(position);
    cursor.setPosition(position + added, QTextCursor::KeepAnchor);
    m_selfEdit = true;
    cursor.mergeCharFormat(m_pendingFormat);
    if (m_pendingClearForeground)
        clearForegroundIn(m_document, position, position + added);
    m_selfEdit = false;
    clearPending();
}

void DocumentController::toggleBold() { toggle(Attr::Bold); }
void DocumentController::toggleItalic() { toggle(Attr::Italic); }
void DocumentController::toggleUnderline() { toggle(Attr::Underline); }
bool DocumentController::isBold() const { return attrState(Attr::Bold); }
bool DocumentController::isItalic() const { return attrState(Attr::Italic); }
bool DocumentController::isUnderline() const { return attrState(Attr::Underline); }

void DocumentController::setFontFamily(const QString &family)
{
    QTextCharFormat change;
    change.setFontFamilies({family});
    applyCharFormat(change);
}

QString DocumentController::currentFontFamily() const
{
    const QStringList families = currentCharFormat().fontFamilies().toStringList();
    return families.isEmpty() ? m_document->defaultFont().family() : families.constFirst();
}

void DocumentController::setFontSize(qreal pointSize)
{
    if (pointSize <= 0)
        return;
    QTextCharFormat change;
    change.setFontPointSize(pointSize);
    applyCharFormat(change);
}

qreal DocumentController::currentFontSize() const
{
    const QTextCharFormat format = currentCharFormat();
    return format.hasProperty(QTextFormat::FontPointSize) ? format.fontPointSize()
                                                          : m_document->defaultFont().pointSizeF();
}

void DocumentController::setTextColor(const QString &hexColor)
{
    if (hexColor.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) {
        applyCharFormat(QTextCharFormat(), true);
        return;
    }
    const QColor color = QColor::fromString(hexColor);
    if (!color.isValid()) {
        qWarning() << "DocumentController: invalid text color" << hexColor;
        return;
    }
    QTextCharFormat change;
    change.setForeground(color);
    applyCharFormat(change);
}

QString DocumentController::currentTextColor() const
{
    const QTextCharFormat format = currentCharFormat();
    if (!format.hasProperty(QTextFormat::ForegroundBrush) || format.foreground().style() == Qt::NoBrush)
        return QStringLiteral("auto");
    return format.foreground().color().name();
}

// --- Paragraph formatting --------------------------------------------------

// Every block the selection touches, or the cursor's block.
QList<QTextBlock> DocumentController::selectedBlocks() const
{
    const QTextCursor cursor = selectionCursor();
    QList<QTextBlock> blocks;
    const QTextBlock last = m_document->findBlock(cursor.selectionEnd());
    for (QTextBlock block = m_document->findBlock(cursor.selectionStart()); block.isValid(); block = block.next()) {
        blocks.append(block);
        if (block == last)
            break;
    }
    return blocks;
}

void DocumentController::setAlignment(Qt::Alignment alignment)
{
    QTextBlockFormat change;
    change.setAlignment(alignment & Qt::AlignHorizontal_Mask);
    const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
    selectionCursor().mergeBlockFormat(change);
    emit formatChanged();
}

int DocumentController::currentAlignment() const
{
    return int(selectionCursor().blockFormat().alignment() & Qt::AlignHorizontal_Mask);
}

// --- Lists -----------------------------------------------------------------
//
// Qt does not join lists: QTextCursor::createList() always makes a new
// QTextList, even right next to an identical one, and each QTextList numbers
// itself from 1 and is written to ODF as its own <text:list>. So these
// functions reuse an adjacent compatible list where one exists and only
// create a list when there is none. Nesting is QTextListFormat::indent():
// one QTextList per run of siblings at a level.

DocumentController::ListKind DocumentController::listKind(const QTextList *list)
{
    if (!list)
        return ListKind::None;
    switch (list->format().style()) {
    case QTextListFormat::ListDisc:
    case QTextListFormat::ListCircle:
    case QTextListFormat::ListSquare:
        return ListKind::Bullet;
    default:
        return ListKind::Numbered;
    }
}

QTextListFormat::Style DocumentController::listStyle(ListKind kind, int indent)
{
    if (kind == ListKind::Numbered)
        return QTextListFormat::ListDecimal;
    static const QTextListFormat::Style bullets[] = {
        QTextListFormat::ListDisc, QTextListFormat::ListCircle, QTextListFormat::ListSquare};
    return bullets[std::max(0, indent - 1) % 3];
}

DocumentController::ListKind DocumentController::currentListKind() const
{
    return listKind(selectionCursor().block().textList());
}

namespace {

int listIndent(const QTextBlock &block)
{
    return block.textList() ? block.textList()->format().indent() : 0;
}

// Takes `block` out of its list as a plain paragraph. QTextList::remove()
// would instead add the list's indent to the block's own, leaving it
// indented as if it were still a list item.
void removeFromList(const QTextBlock &block)
{
    QTextBlockFormat format = block.blockFormat();
    format.setObjectIndex(-1);
    QTextCursor(block).setBlockFormat(format);
}

} // namespace

void DocumentController::toggleList(ListKind kind)
{
    const QList<QTextBlock> blocks = selectedBlocks();
    const bool remove = currentListKind() == kind;
    const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
    QTextCursor edit(m_document);
    edit.beginEditBlock();

    if (remove) {
        for (const QTextBlock &block : blocks)
            if (block.textList())
                removeFromList(block);
    } else {
        // Join a top-level list of this kind directly before or after the
        // blocks; failing that, start one.
        const auto joinable = [kind](const QTextBlock &block) {
            return block.isValid() && listKind(block.textList()) == kind && listIndent(block) == 1
                ? block.textList() : nullptr;
        };
        QTextList *before = joinable(blocks.constFirst().previous());
        QTextList *after = joinable(blocks.constLast().next());
        QTextList *target = before ? before : after;
        if (!target) {
            QTextListFormat format;
            format.setStyle(listStyle(kind, 1));
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
    }

    edit.endEditBlock();
    emit formatChanged();
}

// Moves each selected list item `delta` levels deeper (+1) or shallower (-1).
void DocumentController::changeListLevel(int delta)
{
    const QList<QTextBlock> blocks = selectedBlocks();
    const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
    QTextCursor edit(m_document);
    edit.beginEditBlock();

    for (const QTextBlock &block : blocks) {
        QTextList *list = block.textList();
        if (!list)
            continue;
        const ListKind kind = listKind(list);
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
                if (listKind(previous.textList()) == kind)
                    dest = previous.textList();
                break;
            }
        }
        const QTextBlock next = block.next();
        if (!dest && listIndent(next) == target && listKind(next.textList()) == kind)
            dest = next.textList();

        if (dest) {
            dest->add(block);
        } else {
            QTextListFormat format = list->format();
            format.setIndent(target);
            format.setStyle(listStyle(kind, target));
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
    emit formatChanged();
}

void DocumentController::toggleBulletList() { toggleList(ListKind::Bullet); }
void DocumentController::toggleNumberedList() { toggleList(ListKind::Numbered); }
bool DocumentController::isInBulletList() const { return currentListKind() == ListKind::Bullet; }
bool DocumentController::isInNumberedList() const { return currentListKind() == ListKind::Numbered; }
void DocumentController::promoteListItem() { changeListLevel(-1); }
void DocumentController::demoteListItem() { changeListLevel(+1); }

// --- Tables ----------------------------------------------------------------
//
// QTextTable has no "next cell" operation and the TextEdit owns the visible
// cursor, so these compute the target cell here and ask the TextEdit to
// move there via cursorPositionRequested().

namespace {

constexpr int kMaxTableSize = 50; // the LibreOfficeKit engine's limit

// The cell after (or before) `cell` in reading order, visiting a spanned
// cell once, at its top-left; invalid past either end of the table.
QTextTableCell adjacentCell(QTextTable *table, const QTextTableCell &cell, bool forward)
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

void DocumentController::insertTable(int rows, int columns)
{
    QTextTableFormat format;
    format.setBorder(1);
    format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    format.setBorderCollapse(true);
    format.setCellSpacing(0);
    format.setCellPadding(4);
    format.setWidth(QTextLength(QTextLength::PercentageLength, 100));

    QTextCursor cursor = selectionCursor();
    cursor.clearSelection();
    QTextTable *table = nullptr;
    {
        const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
        table = cursor.insertTable(std::clamp(rows, 1, kMaxTableSize),
                                   std::clamp(columns, 1, kMaxTableSize), format);
    }
    // The TextEdit's cursor was pushed past the table; start in its first cell.
    if (table)
        emit cursorPositionRequested(table->cellAt(0, 0).firstCursorPosition().position());
    emit formatChanged();
}

bool DocumentController::isInTable() const
{
    return selectionCursor().currentTable() != nullptr;
}

// Inserts a row/column after the cursor's cell, or deletes the cursor's
// row/column unless it is the last one: QTextTable::removeRows() and
// removeColumns() delete the whole table when asked to remove every row or
// column.
void DocumentController::editTable(bool rows, bool insert)
{
    const QTextCursor cursor = selectionCursor();
    QTextTable *table = cursor.currentTable();
    if (!table)
        return;
    const QTextTableCell cell = table->cellAt(cursor);
    if (!cell.isValid() || (!insert && (rows ? table->rows() : table->columns()) <= 1))
        return;
    const int row = cell.row(), column = cell.column();
    {
        const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
        if (insert && rows)
            table->insertRows(row + cell.rowSpan(), 1);
        else if (insert)
            table->insertColumns(column + cell.columnSpan(), 1);
        else if (rows)
            table->removeRows(row, 1);
        else
            table->removeColumns(column, 1);
    }
    // The cursor's cell is gone: move to the one that took its place.
    if (!insert) {
        const QTextTableCell landing = table->cellAt(std::min(row, table->rows() - 1),
                                                     std::min(column, table->columns() - 1));
        emit cursorPositionRequested(landing.firstCursorPosition().position());
    }
    emit formatChanged();
}

void DocumentController::insertTableRow() { editTable(true, true); }
void DocumentController::deleteTableRow() { editTable(true, false); }
void DocumentController::insertTableColumn() { editTable(false, true); }
void DocumentController::deleteTableColumn() { editTable(false, false); }

void DocumentController::nextTableCell()
{
    const QTextCursor cursor = selectionCursor();
    QTextTable *table = cursor.currentTable();
    if (!table)
        return;
    QTextTableCell next = adjacentCell(table, table->cellAt(cursor), true);
    if (!next.isValid()) {
        const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
        table->appendRows(1);
        next = table->cellAt(table->rows() - 1, 0);
    }
    emit cursorPositionRequested(next.firstCursorPosition().position());
    emit formatChanged();
}

void DocumentController::previousTableCell()
{
    const QTextCursor cursor = selectionCursor();
    QTextTable *table = cursor.currentTable();
    if (!table)
        return;
    const QTextTableCell previous = adjacentCell(table, table->cellAt(cursor), false);
    if (!previous.isValid())
        return;
    emit cursorPositionRequested(previous.firstCursorPosition().position());
    emit formatChanged();
}
