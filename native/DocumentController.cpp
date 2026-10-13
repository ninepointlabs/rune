#include "DocumentController.h"

#include "DocxBridge.h"
#include "DocxData.h"
#include "HeaderFooter.h"
#include "OdfReader.h"
#include "OdfStyles.h"
#include "OdfWriter.h"
#include "Tables.h"

#include <QAbstractTextDocumentLayout>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFontInfo>
#include <QFutureWatcher>
#include <QLocale>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <memory>

namespace {

// By its real family name: Qt's ODF writer writes the default font into
// every unformatted run, and an unresolved default font calls itself
// "Sans", which other programs (and Rune, reopening) take for another font.
QFont defaultFont(const QTextDocument &doc, qreal pointSize)
{
    QFont font = doc.defaultFont();
    font.setFamily(QFontInfo(font).family());
    font.setPointSizeF(pointSize);
    return font;
}

} // namespace

DocumentController::DocumentController(QObject *parent)
    : QObject(parent), m_parts(new DocumentParts(defaultFont(QTextDocument(), kDefaultPointSize), this)),
      m_document(m_parts->body()), m_margins(new HeaderFooter(*m_parts, this)), m_data(new DocumentData(this)),
      m_edit(m_document),
      m_format(m_edit), m_stream(m_edit)
{
    connect(m_margins, &HeaderFooter::changed, this, &DocumentController::pagePartsChanged);

    // Not wired straight to dirtyChanged(): QTextDocument::clear() resets
    // the modified flag without emitting modificationChanged(), so loads
    // and newDocument() also call updateDirty(). (It drops the undo history
    // just as silently; newDocument() covers that too.) Undo, redo and
    // typing are routed to whichever part is being edited.
    connect(m_parts, &DocumentParts::modificationChanged, this, &DocumentController::updateDirty);
    connect(m_parts, &DocumentParts::undoRedoAvailabilityChanged, this, [this](QTextDocument *doc) {
        if (m_edit.target != doc)
            return;
        emit canUndoChanged();
        emit canRedoChanged();
    });
    connect(m_parts, &DocumentParts::contentsChange, this,
            [this](QTextDocument *doc, int position, int removed, int added) {
                if (m_edit.target == doc)
                    onContentsChange(position, removed, added);
            });
    connect(m_parts, &DocumentParts::contentsChanged, this, [this] { ++m_edit.contentRevision; });
    connect(m_data, &DocumentData::modificationChanged, this, &DocumentController::updateDirty);
    connect(m_data, &DocumentData::changed, this, [this] { ++m_edit.contentRevision; });
    connect(m_document, &QTextDocument::contentsChanged, this, &DocumentController::syncPages);
    connect(m_document->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            &DocumentController::syncPages);

    m_defaultLayout = PageLayout::defaultFor(QLocale::system().territory());
    m_reportedLayout = m_defaultLayout;
    writePageLayout(m_defaultLayout);
    m_document->clearUndoRedoStacks();
    m_document->setModified(false);
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

bool DocumentController::isDirty() const
{
    return m_parts->isModified() || m_data->isModified();
}

void DocumentController::updateDirty()
{
    // While blocked, m_dirty keeps what QML last saw; the caller updates
    // it once unblocked (finishConvertedIo()).
    if (signalsBlocked() || isDirty() == m_dirty)
        return;
    m_dirty = !m_dirty;
    emit dirtyChanged();
}

void DocumentController::moved(int position)
{
    if (position >= 0)
        emit cursorPositionRequested(position);
    emit formatChanged();
}

// --- Page layout -----------------------------------------------------------

PageLayout DocumentController::pageLayout() const
{
    return PageLayout::fromDocument(m_document).value_or(m_defaultLayout);
}

void DocumentController::writePageLayout(const PageLayout &layout)
{
    {
        const auto selfEdit = m_edit.selfEditing();
        layout.storeIn(m_document);
    }
    syncPages();
}

void DocumentController::syncPages()
{
    const PageLayout layout = pageLayout();
    const QSizeF size = layout.sizePx();
    if (m_document->pageSize() != size)
        m_document->setPageSize(size);
    m_margins->syncWidths(layout);
    if (layout != m_reportedLayout) {
        m_reportedLayout = layout;
        emit pageLayoutChanged();
        emit pagePartsChanged(); // their positions
    }
    const int pages = std::max(1, m_document->pageCount());
    if (pages != m_pageCount) {
        m_pageCount = pages;
        m_margins->setPageCount(pages);
        emit pageCountChanged();
        emit pagePartsChanged();
    }
}

void DocumentController::setPageLayout(const QVariantMap &layout)
{
    const PageLayout current = pageLayout();
    const PageLayout next = m_margins->withRoom(PageLayout::fromMap(layout, current));
    if (next == current)
        return;
    // Its own undo step, not merged into the word being typed.
    m_format.endTypingStep();
    writePageLayout(next);
    emit formatChanged();
}

bool DocumentController::localeUsesLetter() const
{
    return PageLayout::usesLetter(QLocale::system().territory());
}

// --- Header and footer -----------------------------------------------------

QTextDocument *DocumentController::partDocument(const QString &part) const { return m_parts->document(part); }
bool DocumentController::hasHeader() const { return m_margins->has(false); }
bool DocumentController::hasFooter() const { return m_margins->has(true); }
qreal DocumentController::headerHeightPx() const { return m_margins->heightPx(false); }
qreal DocumentController::footerHeightPx() const { return m_margins->heightPx(true); }
qreal DocumentController::headerYPx() const { return m_margins->headerYPx(pageLayout()); }
qreal DocumentController::footerYPx() const { return m_margins->footerYPx(pageLayout()); }

void DocumentController::setPartDocument(const QString &part, QQuickTextDocument *qmlDocument)
{
    if (HeaderFooter::isPart(part))
        m_margins->install(HeaderFooter::isFooter(part), qmlDocument, pageLayout());
}

int DocumentController::insertPageField(const QString &part, int position, bool count)
{
    if (!HeaderFooter::isPart(part))
        return position;
    return m_margins->insertPageField(HeaderFooter::isFooter(part), position, count);
}

bool DocumentController::allows(DocumentParts::Capability capability) const
{
    return DocumentParts::capabilities(m_parts->kind(m_parts->idOf(m_edit.target))) & capability;
}

void DocumentController::setEditTarget(const QString &part)
{
    QTextDocument *next = m_parts->document(part);
    if (!next || next == m_edit.target)
        return;
    m_edit.target = next;
    m_format.clearPending();
    m_format.endTypingStep();
    // The new document's selection arrives through setSelection().
    m_edit.cursorPosition = m_edit.selectionStart = m_edit.selectionEnd = 0;
    emit canUndoChanged();
    emit canRedoChanged();
    emit formatChanged();
}

void DocumentController::setPartEditing(int page, const QColor &color)
{
    m_margins->setEditing(page, color);
}

void DocumentController::fitPageParts()
{
    const PageLayout current = pageLayout();
    const PageLayout next = m_margins->withRoom(current);
    if (next == current)
        return;
    m_format.endTypingStep();
    writePageLayout(next);
    emit formatChanged();
}

void DocumentController::paintPart(QPainter *painter, const QString &part, int page, const QColor &color)
{
    if (HeaderFooter::isPart(part))
        m_margins->paint(painter, HeaderFooter::isFooter(part), page, color);
}

void DocumentController::setAllModified(bool modified)
{
    m_parts->setModified(modified);
    m_data->setModified(modified);
}

// --- New, open, save -------------------------------------------------------

void DocumentController::setQmlDocument(QQuickTextDocument *qmlDocument)
{
    if (m_qmlDocument == qmlDocument)
        return;
    m_qmlDocument = qmlDocument;
    if (m_qmlDocument)
        m_qmlDocument->setTextDocument(m_document);
    // The TextEdit may have set its own layout and margins on the document.
    connect(m_document->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            &DocumentController::syncPages, Qt::UniqueConnection);
    syncPages();
    emit documentChanged();
    emit formatChanged();
}

void DocumentController::newDocument()
{
    setEditTarget(QString());
    m_document->clear();
    m_margins->clear();
    m_data->replace({});
    // clear() resets the root frame format, where the page layout lives.
    writePageLayout(m_defaultLayout);
    m_document->clearUndoRedoStacks();
    setAllModified(false);
    updateDirty();
    // clear() also drops the undo history without emitting undoAvailable().
    emit canUndoChanged();
    emit canRedoChanged();
    m_format.clearPending();
    if (!m_currentPath.isEmpty()) {
        m_currentPath.clear();
        emit currentPathChanged();
    }
    emit formatChanged();
}

bool DocumentController::saveToOdf(const QString &path)
{
    OdfWriter::Document document;
    document.body = m_document;
    document.layout = m_margins->asDrawn(pageLayout());
    document.header = m_margins->document(false);
    document.footer = m_margins->document(true);
    document.data = m_data->stores();
    const QByteArray odt = OdfWriter::write(document);
    if (odt.isEmpty())
        return false;
    // Committed atomically: a failed write leaves any existing file as it was.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(odt) != odt.size() || !file.commit()) {
        qWarning() << "DocumentController: cannot write" << path << file.errorString();
        return false;
    }

    setAllModified(false);
    // Undo back to here must stop at the save, not run on into the word.
    m_format.endTypingStep();
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
    setEditTarget(QString());
    {
        // Not typing: keep a pending format from being applied to the load.
        const auto selfEdit = m_edit.selfEditing();
        read = OdfReader::readInto(m_document, path, &error);
    }
    if (!read) {
        qWarning().noquote() << "DocumentController: cannot open" << error;
        return false;
    }
    // Its header and footer, if any.
    for (const bool footer : {false, true}) {
        QString partError;
        QTextDocument *part = m_margins->document(footer);
        if (!OdfReader::readPagePart(part, path, footer, &partError)) {
            qWarning().noquote() << "DocumentController: cannot read the" << (footer ? "footer:" : "header:") << partError;
            part->clear();
        }
    }
    // Its data stores, if any.
    DocumentData::Stores stores;
    QString dataError;
    if (!OdfReader::readData(path, &stores, &dataError))
        qWarning().noquote() << "DocumentController: cannot read the data stores:" << dataError;
    m_data->replace(stores);
    // The file's own page layout, if it has one, with room for the header
    // and footer as laid out here: not part of the history.
    const PageLayout layout = OdfStyles::readPageLayoutFile(path).value_or(m_defaultLayout);
    m_margins->syncWidths(layout);
    writePageLayout(m_margins->withRoom(layout));
    m_document->clearUndoRedoStacks();
    emit canUndoChanged();
    emit canRedoChanged();

    setAllModified(false);
    updateDirty();
    m_format.clearPending();
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

void DocumentController::setBusy(bool busy, bool loading)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    m_loading = busy && loading;
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

    setBusy(true, true);
    const auto work = [path] {
        Conversion result;
        result.odt = DocxBridge::convertToOdt(path, &result.error);
        // LibreOffice's conversion drops them: from the .docx itself.
        if (!result.odt.isEmpty())
            result.data = DocxData::read(path);
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
        if (opened)
            m_data->replace(result.data);
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
        const QList<QTextDocument *> docs = m_parts->documents();
        QList<bool> wasModified;
        for (QTextDocument *doc : docs)
            wasModified.append(doc->isModified());
        const bool dataWasModified = m_data->isModified();
        if (!saveToOdf(odt)) {
            // saveToOdf() changed nothing and has said why
            emit fileSaved(false, path, QStringLiteral("cannot write the temporary .odt for %1").arg(path));
            return;
        }
        // Nothing is saved where the user asked yet: undo what saveToOdf()
        // did to our state until the conversion succeeds.
        for (qsizetype i = 0; i < docs.size(); ++i)
            docs[i]->setModified(wasModified[i]);
        m_data->setModified(dataWasModified);
        m_currentPath = oldPath;
    }

    setBusy(true);
    const quint64 revision = m_edit.contentRevision;
    // LibreOffice's conversion drops the data stores: added to its output.
    const DocumentData::Stores stores = m_data->stores();
    const auto work = [dir, odt, path, stores] {
        Conversion result;
        const auto withData = [&stores](const QByteArray &docx) { return DocxData::add(docx, stores); };
        if (!DocxBridge::convertOdtToDocx(odt, path, &result.error, withData) && result.error.isEmpty())
            result.error = QStringLiteral("conversion failed");
        return result;
    };
    runConversion(work, [this, path, revision](const Conversion &result) {
        if (!result.error.isEmpty()) {
            qWarning().noquote() << "DocumentController: cannot save" << path << "-" << result.error;
        } else {
            const QString oldPath = m_currentPath;
            if (m_edit.contentRevision == revision) {
                const QSignalBlocker blocker(this);
                setAllModified(false);
                m_format.endTypingStep();
            } // else edited since the snapshot: still dirty
            finishConvertedIo(path, oldPath);
        }
        setBusy(false);
        emit fileSaved(result.error.isEmpty(), path, result.error);
    });
}

void DocumentController::finishConvertedIo(const QString &path, const QString &oldPath)
{
    // openOdf()/saveToOdf() ran with this object's signals blocked.
    emit pageLayoutChanged();
    emit pageCountChanged();
    emit pagePartsChanged();
    m_currentPath = QFileInfo(path).absoluteFilePath();
    if (m_currentPath != oldPath)
        emit currentPathChanged();
    updateDirty();
}

// --- Selection, history, typing --------------------------------------------

void DocumentController::setSelection(int cursorPosition, int selectionStart, int selectionEnd)
{
    m_edit.cursorPosition = cursorPosition;
    m_edit.selectionStart = selectionStart;
    m_edit.selectionEnd = selectionEnd;
    m_format.selectionMoved();
    emit formatChanged();
}

void DocumentController::undo() { stepHistory(true); }
void DocumentController::redo() { stepHistory(false); }

void DocumentController::stepHistory(bool undo)
{
    if (m_loading || isStreamingEdit() || !(undo ? canUndo() : canRedo()))
        return;
    QTextCursor cursor = m_edit.selectionCursor();
    {
        // Not typing: keep a pending format from being applied to restored text.
        const auto selfEdit = m_edit.selfEditing();
        if (undo)
            m_edit.target->undo(&cursor);
        else
            m_edit.target->redo(&cursor);
    }
    m_format.clearPending();
    moved(cursor.position());
}

bool DocumentController::typeText(const QString &text)
{
    if (m_loading || isStreamingEdit() || text.isEmpty() || !allows(DocumentParts::WordUndoSteps))
        return false;
    moved(m_format.typeText(text));
    return true;
}

void DocumentController::onContentsChange(int position, int removed, int added)
{
    m_format.onContentsChange(position, removed, added);
}

// --- Character formatting --------------------------------------------------

void DocumentController::toggle(CharFormatter::Attr attr)
{
    m_format.toggle(attr);
    emit formatChanged();
}

void DocumentController::toggleBold() { toggle(CharFormatter::Attr::Bold); }
void DocumentController::toggleItalic() { toggle(CharFormatter::Attr::Italic); }
void DocumentController::toggleUnderline() { toggle(CharFormatter::Attr::Underline); }
bool DocumentController::isBold() const { return m_format.attrState(CharFormatter::Attr::Bold); }
bool DocumentController::isItalic() const { return m_format.attrState(CharFormatter::Attr::Italic); }
bool DocumentController::isUnderline() const { return m_format.attrState(CharFormatter::Attr::Underline); }

void DocumentController::setFontFamily(const QString &family)
{
    m_format.setFontFamily(family);
    emit formatChanged();
}

QString DocumentController::currentFontFamily() const { return m_format.fontFamily(); }

void DocumentController::setFontSize(qreal pointSize)
{
    if (pointSize <= 0)
        return;
    m_format.setFontSize(pointSize);
    emit formatChanged();
}

qreal DocumentController::currentFontSize() const { return m_format.fontSize(); }

void DocumentController::setTextColor(const QString &hexColor)
{
    if (!m_format.setTextColor(hexColor)) {
        qWarning() << "DocumentController: invalid text color" << hexColor;
        return;
    }
    emit formatChanged();
}

QString DocumentController::currentTextColor() const { return m_format.textColor(); }
QVariantList DocumentController::fontFamilies() const { return CharFormatter::installedFamilies(); }

// --- Paragraph formatting --------------------------------------------------

void DocumentController::setAlignment(Qt::Alignment alignment)
{
    QTextBlockFormat change;
    change.setAlignment(alignment & Qt::AlignHorizontal_Mask);
    const auto selfEdit = m_edit.selfEditing();
    m_edit.selectionCursor().mergeBlockFormat(change);
    emit formatChanged();
}

int DocumentController::currentAlignment() const
{
    return int(m_edit.selectionCursor().blockFormat().alignment() & Qt::AlignHorizontal_Mask);
}

// --- Lists -----------------------------------------------------------------

void DocumentController::toggleList(Lists::Kind kind)
{
    if (!allows(DocumentParts::Lists))
        return;
    const QList<QTextBlock> blocks = m_edit.selectedBlocks();
    {
        const auto selfEdit = m_edit.selfEditing();
        if (Lists::kind(m_edit.selectionCursor().block().textList()) == kind)
            Lists::remove(blocks);
        else
            Lists::add(blocks, kind);
    }
    emit formatChanged();
}

void DocumentController::changeListLevel(int delta)
{
    if (!allows(DocumentParts::Lists))
        return;
    {
        const auto selfEdit = m_edit.selfEditing();
        Lists::changeLevel(m_edit.selectedBlocks(), delta);
    }
    emit formatChanged();
}

void DocumentController::toggleBulletList() { toggleList(Lists::Kind::Bullet); }
void DocumentController::toggleNumberedList() { toggleList(Lists::Kind::Numbered); }
bool DocumentController::isInBulletList() const
{
    return Lists::kind(m_edit.selectionCursor().block().textList()) == Lists::Kind::Bullet;
}
bool DocumentController::isInNumberedList() const
{
    return Lists::kind(m_edit.selectionCursor().block().textList()) == Lists::Kind::Numbered;
}
void DocumentController::promoteListItem() { changeListLevel(-1); }
void DocumentController::demoteListItem() { changeListLevel(+1); }

// --- Tables ----------------------------------------------------------------

void DocumentController::insertTable(int rows, int columns)
{
    if (!allows(DocumentParts::Tables))
        return;
    int position = -1;
    {
        const auto selfEdit = m_edit.selfEditing();
        position = Tables::insert(m_edit.selectionCursor(), rows, columns);
    }
    // The TextEdit's cursor was pushed past the table; start in its first cell.
    moved(position);
}

bool DocumentController::isInTable() const
{
    return m_edit.selectionCursor().currentTable() != nullptr;
}

void DocumentController::editTable(bool rows, bool insert, bool before)
{
    if (!allows(DocumentParts::Tables) || !isInTable())
        return;
    int position = -1;
    {
        const auto selfEdit = m_edit.selfEditing();
        position = Tables::edit(m_edit.selectionCursor(), rows, insert, before);
    }
    moved(position);
}

void DocumentController::insertTableRow() { editTable(true, true); }
void DocumentController::insertTableRowAbove() { editTable(true, true, true); }
void DocumentController::deleteTableRow() { editTable(true, false); }
void DocumentController::insertTableColumn() { editTable(false, true); }
void DocumentController::insertTableColumnLeft() { editTable(false, true, true); }
void DocumentController::deleteTableColumn() { editTable(false, false); }

void DocumentController::deleteTable()
{
    if (!allows(DocumentParts::Tables) || !isInTable())
        return;
    int position = -1;
    {
        const auto selfEdit = m_edit.selfEditing();
        position = Tables::remove(m_edit.selectionCursor());
    }
    moved(position);
}

void DocumentController::nextTableCell()
{
    if (!allows(DocumentParts::Tables) || !isInTable())
        return;
    int position = -1;
    {
        const auto selfEdit = m_edit.selfEditing();
        position = Tables::adjacentCell(m_edit.selectionCursor(), true);
    }
    moved(position);
}

void DocumentController::previousTableCell()
{
    if (!allows(DocumentParts::Tables) || !isInTable())
        return;
    const int position = Tables::adjacentCell(m_edit.selectionCursor(), false);
    if (position >= 0)
        moved(position);
}

// --- Streamed edits (AI replies) -------------------------------------------

bool DocumentController::beginStreamedEdit()
{
    if (m_busy || !allows(DocumentParts::Ai) || !m_stream.begin())
        return false;
    m_format.clearPending();
    emit streamingEditChanged();
    emit formatChanged();
    return true;
}

void DocumentController::appendStreamedText(const QString &text)
{
    const int position = m_stream.append(text);
    if (position >= 0)
        emit cursorPositionRequested(position);
}

void DocumentController::finishStreamedEdit(const QString &markdown)
{
    if (!isStreamingEdit())
        return;
    const int position = m_stream.finish(markdown);
    emit cursorPositionRequested(position);
    emit streamingEditChanged();
    emit formatChanged();
}

bool DocumentController::applyStreamedReplacement(const QString &find, const QString &replace)
{
    const int position = m_stream.applyReplacement(find, replace);
    if (position < 0)
        return false;
    emit cursorPositionRequested(position);
    return true;
}

void DocumentController::abortStreamedEdit(bool keepText)
{
    if (!isStreamingEdit())
        return;
    const int position = m_stream.abort(keepText);
    emit cursorPositionRequested(position);
    emit streamingEditChanged();
    emit formatChanged();
}
