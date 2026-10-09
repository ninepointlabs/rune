#include "DocumentController.h"

#include "DocxBridge.h"
#include "OdfReader.h"

#include <QBuffer>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QList>
#include <QScopedValueRollback>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocumentWriter>
#include <QTextFrame>
#include <QTextList>
#include <QRegularExpression>
#include <QTextDocumentFragment>
#include <QTextTable>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <memory>

DocumentController::DocumentController(QObject *parent)
    : QObject(parent), m_document(new QTextDocument(this))
{
    // Not wired straight to dirtyChanged(): QTextDocument::clear() resets
    // the modified flag without emitting modificationChanged(), so loads
    // and newDocument() also call updateDirty(). (It drops the undo history
    // just as silently; newDocument() covers that too.)
    connect(m_document, &QTextDocument::modificationChanged, this, &DocumentController::updateDirty);
    connect(m_document, &QTextDocument::undoAvailable, this, &DocumentController::canUndoChanged);
    connect(m_document, &QTextDocument::redoAvailable, this, &DocumentController::canRedoChanged);
    connect(m_document, &QTextDocument::contentsChange, this, &DocumentController::onContentsChange);
    connect(m_document, &QTextDocument::contentsChanged, this, [this] { ++m_contentRevision; });
}

DocumentController::~DocumentController()
{
    // An open still converting won't reach finishOpen(); remove its
    // temporary .odt whenever it finishes. (A save's temporary directory
    // is owned by its worker.)
    if (m_pendingOpen.isValid())
        m_pendingOpen.then([](const Conversion &result) { DocxBridge::removeConvertedOdt(result.odt); });
    // The TextEdit may outlive us during QML teardown and still points at
    // m_document; hand the document to its wrapper so they die together.
    if (m_qmlDocument)
        m_document->setParent(m_qmlDocument);
}

void DocumentController::updateDirty()
{
    // While blocked, m_dirty keeps what QML last saw; the caller updates
    // it once unblocked (finishConvertedIo()).
    if (signalsBlocked() || m_document->isModified() == m_dirty)
        return;
    m_dirty = !m_dirty;
    emit dirtyChanged();
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
    updateDirty();
    // clear() also drops the undo history without emitting undoAvailable().
    emit canUndoChanged();
    emit canRedoChanged();
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
    // Undo back to here must stop at the save, not run on into the word.
    endTypingStep();
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (m_currentPath != absolute) {
        m_currentPath = absolute;
        emit currentPathChanged();
    }
    return true;
}

bool DocumentController::openOdf(const QString &path)
{
    QString error;
    bool read = false;
    {
        // Not typing: keep a pending format from being applied to the load.
        const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
        read = OdfReader::readInto(m_document, path, &error);
    }
    if (!read) {
        qWarning().noquote() << "DocumentController: cannot open" << error;
        return false;
    }

    m_document->setModified(false);
    updateDirty();
    clearPending();
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (m_currentPath != absolute) {
        m_currentPath = absolute;
        emit currentPathChanged();
    }
    emit formatChanged();
    return true;
}

// --- Other formats via DocxBridge ------------------------------------------
//
// The temporary .odt is opened/saved by the existing openOdf()/saveToOdf()
// with this object's signals blocked, so QML never sees the temporary path;
// finishConvertedIo() then sets the real path and emits once what openOdf()/
// saveToOdf() would have for it. (The document's own signals aren't blocked:
// the TextEdit still sees the content change.)
//
// Only the soffice run (DocxBridge::convertToOdt()/convertOdtToDocx()) leaves
// this thread; everything touching m_document stays here.

namespace {

const QString kAlreadyBusy = QStringLiteral("a file operation is already in progress");

} // namespace

void DocumentController::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

template<typename Work, typename Done>
QFuture<DocumentController::Conversion> DocumentController::runConversion(Work work, Done done)
{
    // Parented to this: if we are destroyed first, so is the watcher, and
    // `done` (which uses this) never runs.
    auto *watcher = new QFutureWatcher<Conversion>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [watcher, done] {
        watcher->deleteLater();
        done(watcher->result());
    });
    const QFuture<Conversion> future = QtConcurrent::run(work);
    watcher->setFuture(future);
    return future;
}

void DocumentController::openFile(const QString &path)
{
    if (m_busy) {
        emit fileOpened(false, path, kAlreadyBusy);
        return;
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("odt")) {
        const bool opened = openOdf(path);
        emit fileOpened(opened, path, opened ? QString() : QStringLiteral("cannot read %1 as an OpenDocument text").arg(path));
        return;
    }
    if (suffix != QLatin1String("docx") && suffix != QLatin1String("doc")) {
        const QString error = QStringLiteral("unsupported file type (expected .odt, .docx or .doc)");
        qWarning().noquote() << "DocumentController: cannot open" << path << "-" << error;
        emit fileOpened(false, path, error);
        return;
    }

    setBusy(true);
    const auto work = [path] {
        Conversion result;
        result.odt = DocxBridge::convertToOdt(path, &result.error);
        return result;
    };
    m_pendingOpen = runConversion(work, [this, path](const Conversion &result) {
        m_pendingOpen = {};
        finishOpen(path, result);
    });
}

void DocumentController::finishOpen(const QString &path, const Conversion &result)
{
    if (result.odt.isEmpty()) {
        qWarning().noquote() << "DocumentController: cannot open" << path << "-" << result.error;
        setBusy(false);
        emit fileOpened(false, path, result.error);
        return;
    }
    // Captured now, not at openFile(): the state the open replaces.
    const QString oldPath = m_currentPath;
    bool opened = false;
    {
        const QSignalBlocker blocker(this);
        opened = openOdf(result.odt);
    }
    DocxBridge::removeConvertedOdt(result.odt);
    if (opened) {
        finishConvertedIo(path, oldPath);
        emit formatChanged();
    } // else openOdf() changed nothing and has said why
    setBusy(false);
    emit fileOpened(opened, path,
                    opened ? QString() : QStringLiteral("cannot read LibreOffice's conversion of %1").arg(path));
}

void DocumentController::saveFile(const QString &path)
{
    if (m_busy) {
        emit fileSaved(false, path, kAlreadyBusy);
        return;
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("odt")) {
        const bool saved = saveToOdf(path);
        emit fileSaved(saved, path, saved ? QString() : QStringLiteral("cannot write %1").arg(path));
        return;
    }
    if (suffix != QLatin1String("docx")) {
        const QString error = QStringLiteral("unsupported file type (expected .odt or .docx)");
        qWarning().noquote() << "DocumentController: cannot save" << path << "-" << error;
        emit fileSaved(false, path, error);
        return;
    }

    // Shared with the worker, which may outlive this controller.
    auto dir = std::make_shared<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/rune-save-XXXXXX"));
    if (!dir->isValid()) {
        const QString error = QStringLiteral("no temporary directory: %1").arg(dir->errorString());
        qWarning().noquote() << "DocumentController: cannot save" << path << "-" << error;
        emit fileSaved(false, path, error);
        return;
    }
    const QString odt = dir->filePath(QFileInfo(path).completeBaseName() + QStringLiteral(".odt"));
    {
        const QSignalBlocker blocker(this);
        const QString oldPath = m_currentPath;
        const bool wasDirty = isDirty();
        if (!saveToOdf(odt)) {
            // saveToOdf() changed nothing and has said why
            emit fileSaved(false, path, QStringLiteral("cannot write the temporary .odt for %1").arg(path));
            return;
        }
        // Nothing is saved where the user asked yet: undo what saveToOdf()
        // did to our state until the conversion succeeds.
        m_document->setModified(wasDirty);
        m_currentPath = oldPath;
    }

    setBusy(true);
    const quint64 revision = m_contentRevision;
    const auto work = [dir, odt, path] {
        Conversion result;
        if (!DocxBridge::convertOdtToDocx(odt, path, &result.error) && result.error.isEmpty())
            result.error = QStringLiteral("conversion failed");
        return result;
    };
    runConversion(work, [this, path, revision](const Conversion &result) {
        if (!result.error.isEmpty()) {
            qWarning().noquote() << "DocumentController: cannot save" << path << "-" << result.error;
        } else {
            const QString oldPath = m_currentPath;
            if (m_contentRevision == revision) {
                const QSignalBlocker blocker(this);
                m_document->setModified(false);
                endTypingStep();
            } // else edited since the snapshot: still dirty
            finishConvertedIo(path, oldPath);
        }
        setBusy(false);
        emit fileSaved(result.error.isEmpty(), path, result.error);
    });
}

void DocumentController::finishConvertedIo(const QString &path, const QString &oldPath)
{
    m_currentPath = QFileInfo(path).absoluteFilePath();
    if (m_currentPath != oldPath)
        emit currentPathChanged();
    updateDirty();
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

void DocumentController::undo() { stepHistory(true); }
void DocumentController::redo() { stepHistory(false); }

void DocumentController::stepHistory(bool undo)
{
    if (m_busy || isStreamingEdit() || !(undo ? canUndo() : canRedo()))
        return;
    QTextCursor cursor = selectionCursor();
    {
        // Not typing: keep a pending format from being applied to restored text.
        const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
        if (undo)
            m_document->undo(&cursor);
        else
            m_document->redo(&cursor);
    }
    clearPending();
    emit cursorPositionRequested(cursor.position());
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

bool DocumentController::typeText(const QString &text)
{
    if (m_busy || isStreamingEdit() || text.isEmpty())
        return false;
    QTextCursor cursor = selectionCursor();
    const bool pending = m_pendingPosition >= 0 && m_pendingPosition == cursor.position() && !cursor.hasSelection();
    QTextCharFormat format = cursor.charFormat();
    if (pending) {
        format.merge(m_pendingFormat);
        if (m_pendingClearForeground)
            format.clearForeground();
    }
    // Extend the current step only when nothing else has happened since
    // and this doesn't start a new word.
    const bool startsWord = m_typingEndsInSpace && !text.front().isSpace();
    const bool extend = m_typingEnd >= 0 && m_typingEnd == cursor.position() && !cursor.hasSelection()
        && m_typingRevision == m_contentRevision && !pending && !startsWord;
    {
        const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
        // Separate edit blocks are never merged by Qt; joining the previous
        // one makes this part of its undo step.
        if (extend)
            cursor.joinPreviousEditBlock();
        else
            cursor.beginEditBlock();
        cursor.insertText(text, format);
        cursor.endEditBlock();
    }
    m_typingEnd = cursor.position();
    m_typingEndsInSpace = text.back().isSpace();
    m_typingRevision = m_contentRevision;
    if (pending)
        clearPending();
    emit cursorPositionRequested(cursor.position());
    emit formatChanged();
    return true;
}

void DocumentController::onContentsChange(int position, int removed, int added)
{
    // Text inserted at the pending position. An input-method commit reports
    // its whole paragraph as replaced (e.g. 0, 7, 8 for one character typed
    // at 6), so take the net growth, and require the range to cover it.
    const int inserted = added - removed;
    if (m_selfEdit || m_pendingPosition < 0 || inserted <= 0 || position > m_pendingPosition
        || position + added < m_pendingPosition + inserted)
        return;
    const int start = m_pendingPosition, end = m_pendingPosition + inserted;
    QTextCursor cursor(m_document);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    m_selfEdit = true;
    cursor.mergeCharFormat(m_pendingFormat);
    if (m_pendingClearForeground)
        clearForegroundIn(m_document, start, end);
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

QTextTableFormat DocumentController::tableFormat()
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

void DocumentController::insertTable(int rows, int columns)
{
    const QTextTableFormat format = tableFormat();
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

// --- Streamed edits (AI replies) ------------------------------------------------

namespace {

// The reply's Markdown, minus a code fence wrapped around all of it (which
// models add despite being asked not to).
QString unfence(const QString &markdown)
{
    const QString trimmed = markdown.trimmed();
    if (!trimmed.startsWith(QLatin1String("```")) || !trimmed.endsWith(QLatin1String("```")) || trimmed.size() < 6)
        return markdown;
    const qsizetype firstNewline = trimmed.indexOf(QLatin1Char('\n'));
    if (firstNewline < 0)
        return markdown;
    return trimmed.mid(firstNewline + 1, trimmed.size() - 3 - firstNewline - 1);
}

} // namespace

DocumentController::MarkdownContext DocumentController::markdownContext() const
{
    const auto markdown = [this](int start, int end) {
        if (start >= end)
            return QString();
        QTextCursor range(m_document);
        range.setPosition(start);
        range.setPosition(end, QTextCursor::KeepAnchor);
        return QTextDocumentFragment(range).toMarkdown();
    };
    const QTextCursor cursor = selectionCursor();
    const int end = m_document->characterCount() - 1;
    return {markdown(0, cursor.selectionStart()), markdown(cursor.selectionStart(), cursor.selectionEnd()),
            markdown(cursor.selectionEnd(), end)};
}

bool DocumentController::beginStreamedEdit()
{
    if (m_busy || m_streaming)
        return false;
    const QTextCursor cursor = selectionCursor();
    clearPending();
    m_streaming = true;
    m_streamSelStart = cursor.selectionStart();
    m_streamSelEnd = cursor.selectionEnd();
    m_streamHadSelection = cursor.hasSelection();
    m_streamStart = m_streamEnd = -1;
    m_streamSearchFrom = m_streamSelStart;
    m_streamHasStep = false;
    emit streamingEditChanged();
    emit formatChanged();
    return true;
}

void DocumentController::openStreamStep(QTextCursor &cursor)
{
    if (m_streamHasStep)
        cursor.joinPreviousEditBlock();
    else
        cursor.beginEditBlock();
    m_streamHasStep = true;
}

void DocumentController::startStreamedText()
{
    if (m_streamStart >= 0)
        return;
    QTextCursor cursor(m_document);
    cursor.setPosition(m_streamSelStart);
    if (m_streamHadSelection) {
        // Typing over a selection keeps its format; so does this.
        QTextCursor first(m_document);
        first.setPosition(m_streamSelStart + 1);
        m_streamFormat = first.charFormat();
        const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
        cursor.setPosition(m_streamSelEnd, QTextCursor::KeepAnchor);
        openStreamStep(cursor);
        cursor.removeSelectedText();
        cursor.endEditBlock();
    } else {
        m_streamFormat = cursor.charFormat();
    }
    m_streamStart = m_streamEnd = cursor.position();
}

void DocumentController::appendStreamedText(const QString &text)
{
    if (!m_streaming || text.isEmpty())
        return;
    startStreamedText();
    QTextCursor cursor(m_document);
    cursor.setPosition(m_streamEnd);
    const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
    openStreamStep(cursor);
    cursor.insertText(text, m_streamFormat);
    cursor.endEditBlock();
    m_streamEnd = cursor.position();
    emit cursorPositionRequested(m_streamEnd);
}

void DocumentController::finishStreamedEdit(const QString &markdown)
{
    if (!m_streaming)
        return;
    startStreamedText();
    // Converted in a document of its own, then inserted as a fragment.
    QTextDocument converted;
    converted.setMarkdown(unfence(markdown).trimmed());
    // Markdown has no empty paragraph; the model writes "&nbsp;" for one.
    for (QTextBlock block = converted.begin(); block.isValid(); block = block.next()) {
        if (block.text() == QString(QChar::Nbsp)) {
            QTextCursor nbsp(block);
            nbsp.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            nbsp.removeSelectedText();
        }
    }
    QTextCursor cursor(m_document);
    {
        const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
        cursor.setPosition(m_streamStart);
        openStreamStep(cursor);
        cursor.setPosition(m_streamEnd, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        // Several paragraphs (or a heading, list or table) get paragraphs
        // of their own: inserted mid-paragraph, the fragment's first block
        // would merge into the text before it and its last into the text
        // after. A single plain paragraph stays inline (a rewritten phrase).
        const QTextBlock firstConverted = converted.begin();
        bool hasTable = false;
        for (auto it = converted.rootFrame()->begin(); !it.atEnd() && !hasTable; ++it)
            hasTable = it.currentFrame() != nullptr;
        const bool ownBlocks = converted.blockCount() > 1 || hasTable || firstConverted.textList()
            || firstConverted.blockFormat().headingLevel() > 0;
        if (ownBlocks) {
            // The spaces at a split would start or end a paragraph.
            while (!cursor.atBlockEnd() && m_document->characterAt(cursor.position()).isSpace())
                cursor.deleteChar();
            while (!cursor.atBlockStart() && m_document->characterAt(cursor.position() - 1).isSpace())
                cursor.deletePreviousChar();
        }
        if (ownBlocks && !cursor.atBlockEnd()) {
            cursor.insertBlock();
            cursor.movePosition(QTextCursor::PreviousCharacter);
        }
        if (ownBlocks && !cursor.atBlockStart())
            cursor.insertBlock();
        const int insertedAt = cursor.position();
        const bool atBlockStart = cursor.atBlockStart();
        if (!converted.isEmpty())
            cursor.insertFragment(QTextDocumentFragment(&converted));
        // A fragment's first block merges into the block it lands in and
        // loses its own block format: a leading "# Title" would arrive as
        // body text. Restore its heading level when it starts a block.
        const int heading = firstConverted.blockFormat().headingLevel();
        if (atBlockStart && heading > 0) {
            QTextCursor first(m_document->findBlock(insertedAt));
            QTextBlockFormat format;
            format.setHeadingLevel(heading);
            first.mergeBlockFormat(format);
        }
        cursor.endEditBlock();
    }
    m_streaming = false;
    m_streamStart = m_streamEnd = -1;
    m_streamHasStep = false;
    emit cursorPositionRequested(cursor.position());
    emit streamingEditChanged();
    emit formatChanged();
}

namespace {

// QTextDocument's frame boundaries (tables): never inside a replacement.
bool isFrameBoundary(QChar c)
{
    return c.unicode() == 0xfdd0 || c.unicode() == 0xfdd1; // QTextBeginningOfFrame / QTextEndOfFrame
}

// `find` as a pattern tolerant of what models change when quoting: Markdown
// markup (emphasis, code, list markers, heading hashes) and whitespace.
QRegularExpression loosePattern(const QString &find)
{
    QString text = find;
    static const QRegularExpression lineMarkers(QStringLiteral(R"((^|\n)[ \t]*(?:[-*+]|\d+[.)]|#{1,6})[ \t]+)"));
    text.replace(lineMarkers, QStringLiteral("\\1"));
    static const QRegularExpression emphasis(QStringLiteral(R"(\*\*|__|\*|`)"));
    text.remove(emphasis);
    const QStringList words = text.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
    QStringList escaped;
    for (const QString &w : words)
        escaped.append(QRegularExpression::escape(w));
    return QRegularExpression(escaped.join(QStringLiteral(R"(\s+)")));
}

} // namespace

bool DocumentController::applyStreamedReplacement(const QString &find, const QString &replace)
{
    if (!m_streaming || find.isEmpty() || m_streamStart >= 0)
        return false; // new text was already being written: one kind of edit per reply
    // The scope as text, position for position ('\n' for paragraph breaks).
    const int scopeStart = m_streamHadSelection ? m_streamSelStart : 0;
    const int scopeEnd = m_streamHadSelection ? m_streamSelEnd : m_document->characterCount() - 1;
    QString text;
    text.reserve(scopeEnd - scopeStart);
    for (int i = scopeStart; i < scopeEnd; ++i) {
        const QChar c = m_document->characterAt(i);
        text.append(c == QChar::ParagraphSeparator || c == QChar::LineSeparator ? QChar(u'\n') : c);
    }
    // First match at or after the previous replacement, else the first.
    const int from = std::clamp(m_streamSearchFrom - scopeStart, 0, int(text.size()));
    int at = -1, length = 0;
    const auto exact = [&](int start) {
        const qsizetype i = text.indexOf(find, start);
        if (i >= 0) {
            at = int(i);
            length = int(find.size());
        }
        return i >= 0;
    };
    const QRegularExpression loose = loosePattern(find);
    const auto fuzzy = [&](int start) {
        if (loose.pattern().isEmpty())
            return false;
        const QRegularExpressionMatch m = loose.match(text, start);
        if (m.hasMatch()) {
            at = int(m.capturedStart());
            length = int(m.capturedLength());
        }
        return m.hasMatch();
    };
    if (!exact(from) && !exact(0) && !fuzzy(from) && !fuzzy(0))
        return false;
    const QString found = text.mid(at, length);
    if (std::any_of(found.begin(), found.end(), isFrameBoundary))
        return false;

    // Rewrite only what differs: the common prefix and suffix stay as they
    // are, formatting and all.
    int prefix = 0;
    while (prefix < found.size() && prefix < replace.size() && found[prefix] == replace[prefix])
        ++prefix;
    int suffix = 0;
    while (suffix < found.size() - prefix && suffix < replace.size() - prefix
           && found[found.size() - 1 - suffix] == replace[replace.size() - 1 - suffix])
        ++suffix;
    const int start = scopeStart + at + prefix;
    const int end = scopeStart + at + int(found.size()) - suffix;
    const QString middle = replace.mid(prefix, replace.size() - prefix - suffix);

    QTextCursor cursor(m_document);
    {
        const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
        cursor.setPosition(start);
        openStreamStep(cursor);
        // The text it replaces sets the format (else the text before it).
        QTextCursor formatAt(m_document);
        formatAt.setPosition(start < end ? start + 1 : start);
        const QTextCharFormat format = formatAt.charFormat();
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.insertText(middle, format);
        // Empty paragraphs it made ("leave a line") aren't list items.
        for (QTextBlock block = m_document->findBlock(start); block.isValid() && block.position() <= cursor.position();
             block = block.next()) {
            if (block.text().isEmpty() && block.textList())
                removeFromList(block);
        }
        cursor.endEditBlock();
    }
    const int delta = int(middle.size()) - (end - start);
    if (m_streamHadSelection)
        m_streamSelEnd += delta;
    m_streamSearchFrom = cursor.position() + suffix;
    emit cursorPositionRequested(cursor.position());
    return true;
}

void DocumentController::abortStreamedEdit(bool keepText)
{
    if (!m_streaming)
        return;
    const bool revert = !keepText && m_streamHasStep;
    const int position = revert ? m_streamSelStart : (m_streamEnd >= 0 ? m_streamEnd : m_streamSearchFrom);
    m_streaming = false;
    m_streamStart = m_streamEnd = -1;
    if (revert) {
        // Our step is the last one: nothing else could edit meanwhile.
        const QScopedValueRollback<bool> selfEdit(m_selfEdit, true);
        m_document->undo();
        // Undone, it would otherwise be redoable.
        m_document->clearUndoRedoStacks(QTextDocument::RedoStack);
    }
    m_streamHasStep = false;
    emit cursorPositionRequested(position);
    emit streamingEditChanged();
    emit formatChanged();
}
