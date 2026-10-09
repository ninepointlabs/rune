#include "DocumentController.h"

#include <QBuffer>
#include <QDebug>
#include <QFileInfo>
#include <QList>
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
    m_pendingPosition = -1;
    m_pendingFormat = QTextCharFormat();
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
// the list and the table.
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
    if (m_pendingPosition >= 0 && (selectionStart != selectionEnd || cursorPosition != m_pendingPosition)) {
        m_pendingPosition = -1;
        m_pendingFormat = QTextCharFormat();
    }
    emit formatChanged();
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
    const bool on = !attrState(attr);
    QTextCharFormat change;
    setAttr(change, attr, on);

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
            m_pendingPosition = position;
            emit formatChanged();
            return;
        }
    }
    cursor.mergeCharFormat(change);
    emit formatChanged();
}

void DocumentController::onContentsChange(int position, int removed, int added)
{
    Q_UNUSED(removed);
    if (m_applyingPending || m_pendingPosition < 0 || added <= 0 || position != m_pendingPosition)
        return;
    QTextCursor cursor(m_document);
    cursor.setPosition(position);
    cursor.setPosition(position + added, QTextCursor::KeepAnchor);
    m_applyingPending = true;
    cursor.mergeCharFormat(m_pendingFormat);
    m_applyingPending = false;
    m_pendingPosition = -1;
    m_pendingFormat = QTextCharFormat();
}

void DocumentController::toggleBold() { toggle(Attr::Bold); }
void DocumentController::toggleItalic() { toggle(Attr::Italic); }
void DocumentController::toggleUnderline() { toggle(Attr::Underline); }
bool DocumentController::isBold() const { return attrState(Attr::Bold); }
bool DocumentController::isItalic() const { return attrState(Attr::Italic); }
bool DocumentController::isUnderline() const { return attrState(Attr::Underline); }
