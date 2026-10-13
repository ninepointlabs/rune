#include "DocumentController.h"

#include "Citations.h"
#include "DocxBridge.h"
#include "DocxData.h"
#include "FormatIds.h"
#include "InlineObjects.h"
#include "HeaderFooter.h"
#include "OdfReader.h"
#include "OdfStyles.h"
#include "OdfWriter.h"
#include "Outline.h"
#include "SourceLinks.h"
#include "Sources.h"
#include "StoryNotes.h"
#include "Tables.h"

#include <QAbstractTextDocumentLayout>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFontInfo>
#include <QFutureWatcher>
#include <QLocale>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScopedValueRollback>
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
    m_engine = Citations::Engine::attach(m_document);
    // A checked link is recorded on its source, if it is still there.
    m_links = new SourceLinks(this);
    connect(m_links, &SourceLinks::checked, this, [this](const QString &id, bool ok) {
        QJsonArray sources = m_data->value(Sources::kStore).toArray();
        const qsizetype at = Sources::indexOf(sources, id);
        if (at < 0)
            return;
        sources.replace(at, Sources::withLinkState(sources.at(at).toObject(), ok ? QStringLiteral("ok") : QStringLiteral("broken")));
        m_data->setValue(Sources::kStore, sources);
    });

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
    // A new step replaces whatever could be redone: follow-up steps there are gone.
    connect(m_document, &QTextDocument::undoCommandAdded, this, [this] {
        const int added = m_document->availableUndoSteps();
        m_followSteps.removeIf([added](int step) { return step >= added; });
    });
    connect(m_data, &DocumentData::changed, this, [this](const QString &name) {
        ++m_edit.contentRevision;
        onDataChanged(name);
    });
    // The outline (and word counts) once edits pause; citations' numbers and
    // the bibliography right after each edit (see checkCitations()).
    m_outlineTimer.setSingleShot(true);
    m_outlineTimer.setInterval(250);
    connect(&m_outlineTimer, &QTimer::timeout, this, &DocumentController::refreshOutline);
    connect(m_document, &QTextDocument::contentsChanged, this, [this] {
        m_outlineTimer.start();
        if (!m_citationCheckQueued) {
            m_citationCheckQueued = true;
            QMetaObject::invokeMethod(this, &DocumentController::checkCitations, Qt::QueuedConnection);
        }
    });
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
    // The TextEdit may have replaced the layout, and the objects' drawing with it.
    InlineObjects::installAll(m_document, DocumentParts::Kind::Body);
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
    {
        const QScopedValueRollback<bool> loading(m_loadingData, true);
        m_data->replace({});
    }
    refreshStructure();
    // clear() resets the root frame format, where the page layout lives.
    writePageLayout(m_defaultLayout);
    m_document->clearUndoRedoStacks();
    m_followSteps.clear();
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
    m_writtenStores = document.data;
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
    return readOdf(path, nullptr);
}

bool DocumentController::readOdf(const QString &path, const DocumentData::Stores *external)
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
    // Its data stores, if any (a converted file's come from the original).
    DocumentData::Stores stores;
    QString dataError;
    if (external)
        stores = *external;
    else if (!OdfReader::readData(path, &stores, &dataError))
        qWarning().noquote() << "DocumentController: cannot read the data stores:" << dataError;
    // What its citations cite; then they are the document's own.
    const QJsonObject saved = stores.take(Citations::kSavedStore).toObject();
    {
        const QScopedValueRollback<bool> loading(m_loadingData, true);
        m_data->replace(stores);
    }
    {
        const auto selfEdit = m_edit.selfEditing();
        Citations::resolveSaved(m_document, saved);
    }
    refreshStructure();
    // The file's own page layout, if it has one, with room for the header
    // and footer as laid out here: not part of the history.
    const PageLayout layout = OdfStyles::readPageLayoutFile(path).value_or(m_defaultLayout);
    m_margins->syncWidths(layout);
    writePageLayout(m_margins->withRoom(layout));
    m_document->clearUndoRedoStacks();
    m_followSteps.clear();
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
        opened = readOdf(result.odt, &result.data);
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
    const DocumentData::Stores stores = m_writtenStores;
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
    // Undo puts the bibliography back as it was with the rest.
    m_skipBibliographyUpdate = true;
    {
        // Not typing: keep a pending format from being applied to restored text.
        const auto selfEdit = m_edit.selfEditing();
        QTextDocument *doc = m_edit.target;
        // A follow-up step (a bibliography update, body text after a
        // heading) goes with the edit before it, both ways.
        const bool body = doc == m_document;
        if (undo) {
            const int step = doc->availableUndoSteps();
            doc->undo(&cursor);
            if (body && m_followSteps.contains(step) && doc->isUndoAvailable())
                doc->undo(&cursor);
        } else {
            doc->redo(&cursor);
            if (body && m_followSteps.contains(doc->availableUndoSteps() + 1) && doc->isRedoAvailable())
                doc->redo(&cursor);
        }
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
    // A paragraph break typed at the end of a heading: the next paragraph
    // is body text, as in any word processor (fixed once the edit is done).
    if (m_edit.selfEdit || m_edit.target != m_document || added <= removed)
        return;
    for (int i = position; i < position + added && i < m_document->characterCount(); ++i)
        if (m_document->characterAt(i) == QChar::ParagraphSeparator) {
            const int after = position + added;
            QMetaObject::invokeMethod(this, [this, after] { bodyAfterHeading(after); }, Qt::QueuedConnection);
            break;
        }
}

void DocumentController::bodyAfterHeading(int position)
{
    if (position >= m_document->characterCount())
        return;
    const QTextBlock block = m_document->findBlock(position);
    const QTextBlock previous = block.previous();
    const int level = block.blockFormat().headingLevel();
    if (!block.isValid() || !block.text().isEmpty() || level == 0 || !previous.isValid()
        || previous.blockFormat().headingLevel() != level || previous.text().isEmpty())
        return;
    const auto selfEdit = m_edit.selfEditing();
    QTextCursor cursor(block);
    cursor.beginEditBlock();
    QTextBlockFormat format = block.blockFormat();
    format.setHeadingLevel(0);
    format.clearProperty(FormatIds::kBibliography);
    cursor.setBlockFormat(format);
    QTextCharFormat chars = block.charFormat();
    chars.clearProperty(QTextFormat::FontWeight);
    chars.clearProperty(QTextFormat::FontSizeAdjustment);
    cursor.setBlockCharFormat(chars);
    cursor.endEditBlock();
    m_followSteps.insert(m_document->availableUndoSteps());
    emit formatChanged();
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
    convertMarkdownCitations();
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
    if (keepText)
        convertMarkdownCitations();
    emit cursorPositionRequested(position);
    emit streamingEditChanged();
    emit formatChanged();
}

// --- Headings and the outline ----------------------------------------------

void DocumentController::setHeadingLevel(int level)
{
    if (!allows(DocumentParts::Headings))
        return;
    level = std::clamp(level, 0, 6);
    const QList<QTextBlock> blocks = m_edit.selectedBlocks();
    {
        const auto selfEdit = m_edit.selfEditing();
        QTextCursor edit(m_document);
        edit.beginEditBlock();
        const QTextCharFormat look = Outline::headingLook(level);
        for (const QTextBlock &block : blocks) {
            QTextCursor cursor(block);
            QTextBlockFormat format = block.blockFormat();
            format.setHeadingLevel(level);
            cursor.setBlockFormat(format);
            // The heading's look over its text, or body text's back.
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            QTextCharFormat chars = block.charFormat();
            if (level > 0) {
                cursor.mergeCharFormat(look);
                chars.merge(look);
            } else {
                struct Range { int start, end; QTextCharFormat format; };
                QList<Range> ranges;
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    QTextCharFormat f = it.fragment().charFormat();
                    if (!f.hasProperty(QTextFormat::FontSizeAdjustment) && !f.hasProperty(QTextFormat::FontWeight))
                        continue;
                    f.clearProperty(QTextFormat::FontSizeAdjustment);
                    f.clearProperty(QTextFormat::FontWeight);
                    ranges.append({it.fragment().position(), it.fragment().position() + it.fragment().length(), f});
                }
                for (const Range &r : ranges) {
                    QTextCursor c(m_document);
                    c.setPosition(r.start);
                    c.setPosition(r.end, QTextCursor::KeepAnchor);
                    c.setCharFormat(r.format);
                }
                chars.clearProperty(QTextFormat::FontSizeAdjustment);
                chars.clearProperty(QTextFormat::FontWeight);
            }
            QTextCursor(block).setBlockCharFormat(chars);
        }
        edit.endEditBlock();
    }
    m_format.endTypingStep();
    emit formatChanged();
    refreshOutline();
}

int DocumentController::currentHeadingLevel() const
{
    return m_edit.selectionCursor().blockFormat().headingLevel();
}

void DocumentController::refreshOutline()
{
    m_outlineTimer.stop();
    QVariantList outline;
    for (const Outline::Section &section : Outline::sections(m_document))
        outline.append(QVariantMap{{QStringLiteral("title"), section.title},
                                   {QStringLiteral("level"), section.level},
                                   {QStringLiteral("position"), section.position},
                                   {QStringLiteral("words"), section.words}});
    const int words = Outline::wordCount(m_document);
    if (outline == m_outline && words == m_wordCount)
        return;
    m_outline = outline;
    m_wordCount = words;
    emit outlineChanged();
}

bool DocumentController::moveSection(int from, int to)
{
    if (m_loading || isStreamingEdit() || !allows(DocumentParts::Headings))
        return false;
    bool moved = false;
    {
        const auto selfEdit = m_edit.selfEditing();
        moved = Outline::moveSection(m_document, from, to);
    }
    if (!moved)
        return false;
    m_format.endTypingStep();
    refreshOutline();
    // The moved section's heading, where it went.
    const qsizetype at = from < to ? to - 1 : to;
    const int index = int(std::min<qsizetype>(at, m_outline.size() - 1));
    if (index >= 0)
        emit cursorPositionRequested(m_outline.at(index).toMap().value(QStringLiteral("position")).toInt());
    emit formatChanged();
    return true;
}

// --- Story notes -----------------------------------------------------------

QVariantList DocumentController::notes() const
{
    return m_data->value(StoryNotes::kStore).toArray().toVariantList();
}

QString DocumentController::addNote(const QString &kind, const QString &name, const QString &text)
{
    QJsonArray notes = m_data->value(StoryNotes::kStore).toArray();
    const QString id = StoryNotes::freshId(notes);
    notes.append(QJsonObject{{QStringLiteral("id"), id},
                             {QStringLiteral("kind"), StoryNotes::kinds().contains(kind) ? kind : QStringLiteral("research")},
                             {QStringLiteral("name"), name.trimmed()},
                             {QStringLiteral("text"), text}});
    m_data->setValue(StoryNotes::kStore, notes);
    return id;
}

bool DocumentController::updateNote(const QString &id, const QString &name, const QString &text)
{
    QJsonArray notes = m_data->value(StoryNotes::kStore).toArray();
    const qsizetype at = StoryNotes::indexOf(notes, id);
    if (at < 0)
        return false;
    QJsonObject note = notes.at(at).toObject();
    note.insert(QStringLiteral("name"), name.trimmed());
    note.insert(QStringLiteral("text"), text);
    notes.replace(at, note);
    return m_data->setValue(StoryNotes::kStore, notes);
}

bool DocumentController::removeNote(const QString &id)
{
    QJsonArray notes = m_data->value(StoryNotes::kStore).toArray();
    const qsizetype at = StoryNotes::indexOf(notes, id);
    if (at < 0)
        return false;
    notes.removeAt(at);
    return m_data->setValue(StoryNotes::kStore, notes.isEmpty() ? QJsonValue() : QJsonValue(notes));
}

// --- Sources and citations -------------------------------------------------

QVariantList DocumentController::sources() const
{
    QVariantList out;
    for (const QJsonValue &value : m_data->value(Sources::kStore).toArray()) {
        const QJsonObject item = value.toObject();
        out.append(QVariantMap{{QStringLiteral("id"), item.value(QStringLiteral("id")).toString()},
                               {QStringLiteral("label"), Sources::label(item)},
                               {QStringLiteral("item"), item.toVariantMap()},
                               {QStringLiteral("unreviewed"), Sources::isUnreviewed(item)},
                               {QStringLiteral("link"), Sources::linkState(item)},
                               {QStringLiteral("cited"), m_engine->citedOrder().contains(item.value(QStringLiteral("id")).toString())}});
    }
    return out;
}

QString DocumentController::addSource(const QVariantMap &item)
{
    QJsonArray sources = m_data->value(Sources::kStore).toArray();
    const QJsonObject stored = Sources::normalized(QJsonObject::fromVariantMap(item), sources);
    sources.append(stored);
    m_data->setValue(Sources::kStore, sources);
    return stored.value(QStringLiteral("id")).toString();
}

QString DocumentController::updateSource(const QString &id, const QVariantMap &item)
{
    QJsonArray sources = m_data->value(Sources::kStore).toArray();
    const qsizetype at = Sources::indexOf(sources, id);
    if (at < 0)
        return {};
    QJsonObject updated = QJsonObject::fromVariantMap(item);
    // Kept unless replaced: a person editing it has reviewed it.
    updated = Sources::withUnreviewed(updated, false);
    updated = Sources::normalized(updated, sources, id);
    const QString newId = updated.value(QStringLiteral("id")).toString();
    sources.replace(at, updated);
    m_data->setValue(Sources::kStore, sources);
    // Citations of it follow a changed id.
    if (newId != id)
        renameCitedSource(id, newId);
    return newId;
}

bool DocumentController::removeSource(const QString &id)
{
    QJsonArray sources = m_data->value(Sources::kStore).toArray();
    const qsizetype at = Sources::indexOf(sources, id);
    if (at < 0)
        return false;
    sources.removeAt(at);
    return m_data->setValue(Sources::kStore, sources.isEmpty() ? QJsonValue() : QJsonValue(sources));
}

int DocumentController::importBibTeX(const QString &bibtex)
{
    const QJsonArray incoming = Sources::fromBibTeX(bibtex);
    if (incoming.isEmpty())
        return 0;
    QJsonArray sources = m_data->value(Sources::kStore).toArray();
    // Imported sources never replace existing ones: fresh ids where taken.
    QJsonArray fresh;
    for (const QJsonValue &value : incoming) {
        QJsonObject item = value.toObject();
        if (Sources::indexOf(sources, item.value(QStringLiteral("id")).toString()) >= 0)
            item.remove(QStringLiteral("id"));
        fresh.append(item);
    }
    const QStringList ids = Sources::merge(sources, fresh, false);
    m_data->setValue(Sources::kStore, sources);
    return int(ids.size());
}

bool DocumentController::markSourceReviewed(const QString &id)
{
    QJsonArray sources = m_data->value(Sources::kStore).toArray();
    const qsizetype at = Sources::indexOf(sources, id);
    if (at < 0)
        return false;
    sources.replace(at, Sources::withUnreviewed(sources.at(at).toObject(), false));
    return m_data->setValue(Sources::kStore, sources);
}

QString DocumentController::citationStyle() const
{
    return CitationStyles::id(m_engine->style());
}

void DocumentController::setCitationStyle(const QString &style)
{
    if (!CitationStyles::ids().contains(style) || style == citationStyle())
        return;
    m_data->setValue(Citations::kSettingsStore, QJsonObject{{QStringLiteral("style"), style}});
}

void DocumentController::setInkColor(const QColor &color)
{
    if (color == m_engine->color())
        return;
    m_engine->setColor(color);
    emit inkColorChanged();
}

QVariantList DocumentController::citationStyles() const
{
    QVariantList out;
    for (const QString &id : CitationStyles::ids())
        out.append(QVariantMap{{QStringLiteral("id"), id},
                               {QStringLiteral("name"), CitationStyles::displayName(CitationStyles::fromId(id))}});
    return out;
}

bool DocumentController::insertCitation(const QStringList &ids, const QString &locator)
{
    if (m_loading || isStreamingEdit() || ids.isEmpty() || !allows(DocumentParts::Citations))
        return false;
    QTextCursor cursor = m_edit.selectionCursor();
    const QTextCharFormat base = InlineObjects::withoutObject(cursor.charFormat());
    {
        const auto selfEdit = m_edit.selfEditing();
        cursor.beginEditBlock();
        cursor.insertText(QString(QChar::ObjectReplacementCharacter), Citations::format(ids, locator, base));
        m_engine->refresh();
        Citations::writeBibliography(cursor, false, true);
        cursor.endEditBlock();
    }
    m_format.endTypingStep();
    emit cursorPositionRequested(cursor.position());
    emit formatChanged();
    emit sourcesChanged(); // what is cited
    return true;
}

bool DocumentController::insertBibliography()
{
    if (m_loading || isStreamingEdit() || !allows(DocumentParts::Citations))
        return false;
    m_engine->refresh();
    bool written = false;
    {
        const auto selfEdit = m_edit.selfEditing();
        written = Citations::writeBibliography(m_edit.selectionCursor(), true, false);
    }
    m_format.endTypingStep();
    emit formatChanged();
    return written;
}

QVariantMap DocumentController::citationAtCursor() const
{
    QTextCursor cursor = m_edit.selectionCursor();
    for (const int position : {cursor.selectionStart(), cursor.selectionStart() - 1}) {
        if (position < 0)
            continue;
        QTextCursor c(m_edit.target);
        c.setPosition(position);
        c.setPosition(position + 1, QTextCursor::KeepAnchor);
        const QTextCharFormat format = c.charFormat();
        if (Citations::isCitation(format))
            return {{QStringLiteral("position"), position}, {QStringLiteral("ids"), Citations::ids(format)},
                    {QStringLiteral("locator"), Citations::locator(format)}};
    }
    return {};
}

void DocumentController::renameCitedSource(const QString &from, const QString &to)
{
    QList<QPair<int, QTextCharFormat>> found;
    for (QTextBlock b = m_document->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it)
            if (Citations::isCitation(it.fragment().charFormat()) && Citations::ids(it.fragment().charFormat()).contains(from))
                for (int i = 0; i < it.fragment().length(); ++i)
                    found.append({it.fragment().position() + i, it.fragment().charFormat()});
    if (found.isEmpty())
        return;
    const auto selfEdit = m_edit.selfEditing();
    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    for (const auto &[position, format] : std::as_const(found)) {
        QStringList ids = Citations::ids(format);
        ids.replaceInStrings(QRegularExpression(QStringLiteral("^%1$").arg(QRegularExpression::escape(from))), to);
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        cursor.setCharFormat(Citations::format(ids, Citations::locator(format), format));
    }
    cursor.endEditBlock();
    m_engine->refresh();
}

void DocumentController::onDataChanged(const QString &name)
{
    if (name == Sources::kStore || name == Citations::kSettingsStore) {
        m_engine->setSources(m_data->value(Sources::kStore).toArray());
        m_engine->setStyle(CitationStyles::fromId(
            m_data->value(Citations::kSettingsStore).toObject().value(QStringLiteral("style")).toString()));
        // A source or the style changed: the bibliography follows, as its
        // own undo step (a file being opened has its own already).
        if (!m_loadingData) {
            m_engine->refresh();
            const auto selfEdit = m_edit.selfEditing();
            Citations::writeBibliography(QTextCursor(m_document), false, false);
        }
        emit sourcesChanged();
    } else if (name == StoryNotes::kStore) {
        emit notesChanged();
    }
}

void DocumentController::checkCitations()
{
    m_citationCheckQueued = false;
    m_engine->refresh();
    const bool skip = m_skipBibliographyUpdate || m_loadingData || isStreamingEdit() || m_loading;
    m_skipBibliographyUpdate = false;
    if (skip || !Citations::hasBibliography(m_document))
        return;
    // A citation typed away or pasted: the bibliography follows, in the same
    // undo step as the edit.
    const auto selfEdit = m_edit.selfEditing();
    if (Citations::writeBibliography(QTextCursor(m_document), false, false)) {
        m_followSteps.insert(m_document->availableUndoSteps());
        emit sourcesChanged();
    }
}

void DocumentController::refreshStructure()
{
    m_engine->setSources(m_data->value(Sources::kStore).toArray());
    m_engine->setStyle(CitationStyles::fromId(
        m_data->value(Citations::kSettingsStore).toObject().value(QStringLiteral("style")).toString()));
    m_engine->refresh();
    refreshOutline();
    emit sourcesChanged();
    emit notesChanged();
}

int DocumentController::convertMarkdownCitations()
{
    // Each [@key] whose keys are all sources becomes a citation.
    struct Found { int position; Citations::MarkdownCitation citation; };
    QList<Found> found;
    const QJsonArray sources = m_data->value(Sources::kStore).toArray();
    for (QTextBlock b = m_document->begin(); b.isValid(); b = b.next()) {
        const QString text = b.text();
        if (!text.contains(QStringLiteral("[@")))
            continue;
        for (const Citations::MarkdownCitation &c : Citations::findInMarkdown(text)) {
            const bool known = std::all_of(c.ids.cbegin(), c.ids.cend(),
                                           [&](const QString &id) { return Sources::indexOf(sources, id) >= 0; });
            if (known)
                found.append({b.position() + int(c.start), c});
        }
    }
    if (found.isEmpty())
        return 0;
    const auto selfEdit = m_edit.selfEditing();
    QTextCursor cursor(m_document);
    cursor.joinPreviousEditBlock();
    for (auto it = found.crbegin(); it != found.crend(); ++it) {
        cursor.setPosition(it->position);
        cursor.setPosition(it->position + int(it->citation.length), QTextCursor::KeepAnchor);
        QTextCursor at(m_document);
        at.setPosition(it->position + 1);
        const QTextCharFormat base = InlineObjects::withoutObject(at.charFormat());
        cursor.insertText(QString(QChar::ObjectReplacementCharacter),
                          Citations::format(it->citation.ids, it->citation.locator, base));
    }
    m_engine->refresh();
    Citations::writeBibliography(cursor, false, true);
    cursor.endEditBlock();
    emit sourcesChanged();
    return int(found.size());
}

// --- Agents ----------------------------------------------------------------

QString DocumentController::agentData() const
{
    QString out;
    const auto block = [](const QString &name, const QJsonValue &value) {
        const QJsonDocument json = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
        return QStringLiteral("```") + name + QLatin1Char('\n') + QString::fromUtf8(json.toJson(QJsonDocument::Indented))
            + QStringLiteral("```\n");
    };
    const QJsonValue notes = m_data->value(StoryNotes::kStore);
    if (notes.isArray() && !notes.toArray().isEmpty())
        out += block(StoryNotes::kStore, notes);
    const QJsonValue sources = m_data->value(Sources::kStore);
    if (sources.isArray() && !sources.toArray().isEmpty())
        out += block(Sources::kStore, sources);
    return out;
}

int DocumentController::applyAgentData(const QString &name, const QString &json)
{
    QJsonParseError error;
    const QJsonDocument parsed = QJsonDocument::fromJson(json.toUtf8(), &error);
    QJsonArray incoming = parsed.isArray() ? parsed.array() : QJsonArray();
    if (parsed.isObject())
        incoming.append(parsed.object());
    if (incoming.isEmpty())
        return 0;
    if (name == Sources::kStore) {
        QJsonArray sources = m_data->value(Sources::kStore).toArray();
        const QStringList ids = Sources::merge(sources, incoming, true);
        m_data->setValue(Sources::kStore, sources);
        // Does what the agent cited exist? Its DOI or URL should resolve.
        for (const QString &id : ids) {
            const QUrl link = m_links->linkFor(Sources::find(sources, id));
            if (!link.isEmpty())
                m_links->check(id, link);
        }
        return int(ids.size());
    }
    if (name == StoryNotes::kStore) {
        QJsonArray notes = m_data->value(StoryNotes::kStore).toArray();
        const int changed = StoryNotes::merge(notes, incoming);
        if (changed > 0)
            m_data->setValue(StoryNotes::kStore, notes.isEmpty() ? QJsonValue() : QJsonValue(notes));
        return changed;
    }
    return 0;
}
