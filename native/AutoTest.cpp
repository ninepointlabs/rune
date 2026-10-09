#include "AutoTest.h"

#include "DocumentController.h"
#include "DocxBridge.h"
#include "OdfReader.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QProcess>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocumentWriter>
#include <QTextList>
#include <QTextTable>
#include <QTextStream>
#include <QTimer>
#include <QtTest/QTest>

#include <quazip.h>
#include <quazipfile.h>

#include <algorithm>
#include <functional>
#include <memory>

namespace {

const QString kStage1Path = QStringLiteral("/tmp/rune-native-stage1.odt");
const QString kSanitizePath = QStringLiteral("/tmp/rune-native-sanitize.odt");
const QString kStage2AlignPath = QStringLiteral("/tmp/rune-native-stage2-align.odt");
const QString kStage2ListPath = QStringLiteral("/tmp/rune-native-stage2-list.odt");
const QString kStage2FontPath = QStringLiteral("/tmp/rune-native-stage2-font.odt");
const QString kStage2ColorPath = QStringLiteral("/tmp/rune-native-stage2-color.odt");
const QString kStage3ListTablePath = QStringLiteral("/tmp/rune-native-stage3-list-table.odt");
const QString kStage3ListTableRawPath = QStringLiteral("/tmp/rune-native-stage3-list-table-unsanitized.odt");
const QString kStage3TablePath = QStringLiteral("/tmp/rune-native-stage3-table.odt");
const QString kStage3TabPath = QStringLiteral("/tmp/rune-native-stage3-tab.odt");
const QString kStage4RoundTripPath = QStringLiteral("/tmp/rune-native-stage4-roundtrip.odt");
const QString kStage4ExternalDir = QStringLiteral("/tmp/rune-native-stage4-external");
const QString kStage4MissingPath = QStringLiteral("/tmp/rune-native-stage4-does-not-exist.odt");
const QString kStage4bListPath = QStringLiteral("/tmp/rune-native-stage4b-list.odt");
const QString kStage4bTablePath = QStringLiteral("/tmp/rune-native-stage4b-table.odt");
const QString kStage4bFontPath = QStringLiteral("/tmp/rune-native-stage4b-font.odt");
const QString kStage4bInheritPath = QStringLiteral("/tmp/rune-native-stage4b-inherit.odt");
const QString kStage4NotZipPath = QStringLiteral("/tmp/rune-native-stage4-not-a-zip.odt");
const QString kStage4NoContentPath = QStringLiteral("/tmp/rune-native-stage4-no-content.odt");
const QString kStage4BadXmlPath = QStringLiteral("/tmp/rune-native-stage4-bad-xml.odt");
const QString kStage5DocxPath = QStringLiteral("/tmp/rune-native-stage5-roundtrip.docx");
const QString kStage5TextDir = QStringLiteral("/tmp/rune-native-stage5-txt");
const QString kStage5MissingPath = QStringLiteral("/tmp/rune-native-stage5-does-not-exist.docx");
const QString kStage5TxtPath = QStringLiteral("/tmp/rune-native-stage5-plain.txt");
const QString kStage5NoExtPath = QStringLiteral("/tmp/rune-native-stage5-no-extension");
const QString kStage5CorruptPath = QStringLiteral("/tmp/rune-native-stage5-corrupt.docx");
const QString kStage5UnwritablePath = QStringLiteral("/tmp/rune-native-stage5-no-such-dir/out.docx");
const QString kAsyncOdtPath = QStringLiteral("/tmp/rune-native-async.odt");
const QString kAsyncEditDocxPath = QStringLiteral("/tmp/rune-native-async-edited.docx");
const QString kUndoPath = QStringLiteral("/tmp/rune-native-undo.odt");
const QString kDialogBasePath = QStringLiteral("/tmp/rune-native-dialog");
const QString kDialogOdtPath = kDialogBasePath + QStringLiteral(".odt");
const QString kDialogDocxPath = QStringLiteral("/tmp/rune-native-dialog.docx");
const QString kSampleDocx = QStringLiteral(RUNE_SAMPLES_DIR "/test.docx");
const QString kSampleDoc = QStringLiteral(RUNE_SAMPLES_DIR "/test.doc");

class Checker
{
public:
    void check(bool ok, const QString &what)
    {
        QTextStream(stdout) << (ok ? "  ok    " : "  FAIL  ") << what << Qt::endl;
        m_ok = m_ok && ok;
    }
    bool ok() const { return m_ok; }

private:
    bool m_ok = true;
};

// Real key events through the window, as if typed.
void typeText(QQuickWindow *window, const QString &text)
{
    for (const QChar ch : text) {
        const int key = ch == ' ' ? Qt::Key_Space : ch.toUpper().unicode();
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, QString(ch));
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, QString(ch));
        QCoreApplication::sendEvent(window, &press);
        QCoreApplication::sendEvent(window, &release);
    }
}

// Formatting of the characters [start, end) as "B", "I", "U" flags; "mixed"
// if they differ.
QString formatOf(QTextDocument *doc, int start, int end)
{
    QString result;
    for (int pos = start; pos < end; ++pos) {
        QTextCursor c(doc);
        c.setPosition(pos + 1); // charFormat() is the char before the cursor
        const QTextCharFormat f = c.charFormat();
        QString flags;
        if (f.fontWeight() >= QFont::Bold) flags += 'B';
        if (f.fontItalic()) flags += 'I';
        if (f.fontUnderline()) flags += 'U';
        if (pos == start)
            result = flags;
        else if (flags != result)
            return QStringLiteral("mixed");
    }
    return result;
}

// The block directly before the document's first top-level table.
QTextBlock blockBeforeFirstTable(QTextDocument *doc)
{
    QTextBlock previous;
    for (auto it = doc->rootFrame()->begin(); !it.atEnd(); ++it) {
        if (qobject_cast<QTextTable *>(it.currentFrame()))
            return previous;
        previous = it.currentFrame() ? QTextBlock() : it.currentBlock();
    }
    return {};
}

int listIndent(const QTextBlock &block)
{
    return block.textList() ? block.textList()->format().indent() : 0;
}

// A real key press + release through the window.
void pressKey(QQuickWindow *window, int key, Qt::KeyboardModifiers modifiers, const QString &text)
{
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QCoreApplication::sendEvent(window, &press);
    QCoreApplication::sendEvent(window, &release);
}

// A key press + release through the platform input path, as a real
// keyboard delivers it: unlike sendEvent(), this goes through Qt's shortcut
// handling (ShortcutOverride, then Shortcut items) before the focus item.
void pressSystemKey(QQuickWindow *window, Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    QTest::keyClick(window, key, modifiers);
    QCoreApplication::processEvents();
}

// Real Return key, which TextEdit turns into a new paragraph block.
void pressReturn(QQuickWindow *window)
{
    pressKey(window, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
}

void pressTab(QQuickWindow *window)
{
    pressKey(window, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
}

void pressBacktab(QQuickWindow *window)
{
    pressKey(window, Qt::Key_Backtab, Qt::ShiftModifier, QString());
}

// The table the TextEdit's cursor is in, if any.
QTextTable *tableAtCursor(QTextDocument *doc, QQuickItem *editor)
{
    QTextCursor cursor(doc);
    cursor.setPosition(editor->property("cursorPosition").toInt());
    return cursor.currentTable();
}

// The TextEdit's cursor cell as "row,column", or "none".
QString cellAtCursor(QTextDocument *doc, QQuickItem *editor)
{
    QTextCursor cursor(doc);
    cursor.setPosition(editor->property("cursorPosition").toInt());
    QTextTable *table = cursor.currentTable();
    if (!table)
        return QStringLiteral("none");
    const QTextTableCell cell = table->cellAt(cursor);
    return QStringLiteral("%1,%2").arg(cell.row()).arg(cell.column());
}

// Converts `odtPath` to HTML with a real headless LibreOffice and returns the
// HTML ("" on failure). A private profile keeps it independent of any
// running LibreOffice.
QString convertWithSoffice(const QString &odtPath)
{
    const QString outDir = QStringLiteral("/tmp/rune-native-html");
    QDir().mkpath(outDir);
    const QString htmlPath = outDir + '/' + QFileInfo(odtPath).completeBaseName() + QStringLiteral(".html");
    QFile::remove(htmlPath);
    QProcess soffice;
    soffice.start(QStringLiteral("soffice"),
                  {QStringLiteral("-env:UserInstallation=file:///tmp/rune-native-lo-profile"),
                   QStringLiteral("--headless"), QStringLiteral("--convert-to"), QStringLiteral("html"),
                   QStringLiteral("--outdir"), outDir, odtPath});
    if (!soffice.waitForFinished(120000) || soffice.exitCode() != 0)
        return {};
    QFile html(htmlPath);
    return html.open(QIODevice::ReadOnly) ? QString::fromUtf8(html.readAll()) : QString();
}

int countOf(const QString &html, const QString &tag)
{
    return int(html.count(QRegularExpression(QStringLiteral("<%1[\\s>]").arg(tag),
                                             QRegularExpression::CaseInsensitiveOption)));
}

// The HTML's text content, tags stripped and whitespace collapsed.
QString textOf(const QString &html)
{
    QString body = html.mid(html.indexOf(QLatin1String("<body"), 0, Qt::CaseInsensitive));
    body.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    return body.simplified();
}

// Reports what soffice made of a saved file, for the log.
void logConversion(const QString &odtPath, const QString &html)
{
    QTextStream(stdout) << "  soffice " << QFileInfo(odtPath).fileName() << " -> html: "
                        << countOf(html, QStringLiteral("table")) << " table(s), "
                        << countOf(html, QStringLiteral("tr")) << " <tr>, "
                        << countOf(html, QStringLiteral("td")) << " <td>, "
                        << countOf(html, QStringLiteral("li")) << " <li>; text: \"" << textOf(html) << "\""
                        << Qt::endl;
}

// Converts the HTML file `htmlPath` to .odt with a real headless LibreOffice
// into `outDir`; returns the .odt path ("" on failure).
QString sofficeHtmlToOdt(const QString &htmlPath, const QString &outDir)
{
    QDir().mkpath(outDir);
    const QString odtPath = outDir + '/' + QFileInfo(htmlPath).completeBaseName() + QStringLiteral(".odt");
    QFile::remove(odtPath);
    QProcess soffice;
    soffice.start(QStringLiteral("soffice"),
                  {QStringLiteral("-env:UserInstallation=file:///tmp/rune-native-lo-profile"),
                   QStringLiteral("--headless"), QStringLiteral("--convert-to"), QStringLiteral("odt:writer8"),
                   QStringLiteral("--outdir"), outDir, htmlPath});
    if (!soffice.waitForFinished(120000) || soffice.exitCode() != 0)
        return {};
    return QFileInfo::exists(odtPath) ? odtPath : QString();
}

// A zip holding one file `name` with `data`.
bool writeZip(const QString &path, const QString &name, const QByteArray &data)
{
    QFile::remove(path);
    QuaZip zip(path);
    if (!zip.open(QuaZip::mdCreate))
        return false;
    QuaZipFile file(&zip);
    if (!file.open(QIODevice::WriteOnly, QuaZipNewInfo(name)))
        return false;
    const bool ok = file.write(data) == data.size();
    file.close();
    zip.close();
    return ok && zip.getZipError() == UNZ_OK;
}

// Block texts as a readable list, for the log.
QString blocksOf(QTextDocument *doc)
{
    QStringList blocks;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next())
        blocks << '"' + block.text().replace('\t', QLatin1String("\\t")) + '"';
    return blocks.join(QLatin1String(", "));
}

// Where `word` starts in the plain text, as a document position.
int positionOf(QTextDocument *doc, const QString &word)
{
    return doc->toPlainText().indexOf(word);
}

QTextBlock blockWithText(QTextDocument *doc, const QString &text)
{
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next())
        if (block.text() == text)
            return block;
    return {};
}

// A zip with several entries, for hand-built ODF test fixtures.
bool writeZipMulti(const QString &path, const QList<QPair<QString, QByteArray>> &entries)
{
    QFile::remove(path);
    QuaZip zip(path);
    if (!zip.open(QuaZip::mdCreate))
        return false;
    for (const auto &[name, data] : entries) {
        QuaZipFile file(&zip);
        if (!file.open(QIODevice::WriteOnly, QuaZipNewInfo(name)))
            return false;
        if (file.write(data) != data.size())
            return false;
        file.close();
    }
    zip.close();
    return zip.getZipError() == UNZ_OK;
}

// Converts `path` to UTF-8 plain text with a real headless LibreOffice into
// `outDir` and returns the text ("" on failure). Independent of DocxBridge:
// this is the check that a saved file really is what it claims to be.
QString sofficeToText(const QString &path, const QString &outDir)
{
    QDir().mkpath(outDir);
    const QString txtPath = outDir + '/' + QFileInfo(path).completeBaseName() + QStringLiteral(".txt");
    QFile::remove(txtPath);
    QProcess soffice;
    soffice.start(QStringLiteral("soffice"),
                  {QStringLiteral("-env:UserInstallation=file:///tmp/rune-native-lo-profile"),
                   QStringLiteral("--headless"), QStringLiteral("--convert-to"), QStringLiteral("txt:Text (encoded):UTF8"),
                   QStringLiteral("--outdir"), outDir, path});
    if (!soffice.waitForFinished(120000) || soffice.exitCode() != 0)
        return {};
    QFile txt(txtPath);
    return txt.open(QIODevice::ReadOnly) ? QString::fromUtf8(txt.readAll()) : QString();
}

// Text as a whitespace-collapsed word sequence, without soffice's bullet
// characters and BOM, so its .txt export and toPlainText() compare.
QString wordsOf(QString text)
{
    text.remove(QChar(0xFEFF)).remove(QChar(0x2022));
    return text.simplified();
}

bool listKindIsBullet(const QTextList *list)
{
    const QTextListFormat::Style style = list->format().style();
    return style == QTextListFormat::ListDisc || style == QTextListFormat::ListCircle
        || style == QTextListFormat::ListSquare;
}

// DocxBridge's temporary directories currently in the temp dir.
QStringList bridgeTempDirs()
{
    return QDir(QDir::tempPath()).entryList({QStringLiteral("rune-docx-*"), QStringLiteral("rune-save-*")},
                                            QDir::Dirs | QDir::NoDotAndDotDot);
}

// How one openFile()/saveFile() call went, as seen from outside.
struct IoRun
{
    bool fired = false;          // fileOpened/fileSaved arrived (before the timeout)
    bool success = false;
    QString path;
    QString error;
    bool completedInCall = false; // the signal fired before openFile()/saveFile() returned
    bool busyAfterCall = false;   // controller->isBusy() as the call returned
    bool busyAtSignal = false;    // ... and as the completion signal fired
    int busyChanges = 0;          // busyChanged() emissions, call to completion
    qint64 elapsedMs = 0;         // call to completion
};

// Generous: one soffice run is bounded by DocxBridge::kTimeoutMs.
constexpr int kIoTimeoutMs = DocxBridge::kTimeoutMs + 10000;

// Pumps the event loop until `done()` or `timeoutMs` elapses; true if done.
bool waitUntil(const std::function<bool()> &done, int timeoutMs = kIoTimeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
    return done();
}

// Calls `start` (an openFile()/saveFile() on `controller`) and waits for
// `completion` (&DocumentController::fileOpened or ::fileSaved).
template<typename Signal>
IoRun runIo(DocumentController *controller, Signal completion, const std::function<void()> &start)
{
    IoRun run;
    QElapsedTimer timer;
    bool returned = false;
    const auto busyConn = QObject::connect(controller, &DocumentController::busyChanged, [&] { ++run.busyChanges; });
    const auto doneConn = QObject::connect(controller, completion,
                                           [&](bool success, const QString &path, const QString &error) {
                                               if (run.fired)
                                                   return;
                                               run.fired = true;
                                               run.success = success;
                                               run.path = path;
                                               run.error = error;
                                               run.completedInCall = !returned;
                                               run.busyAtSignal = controller->isBusy();
                                               run.elapsedMs = timer.elapsed();
                                           });
    timer.start();
    start();
    returned = true;
    run.busyAfterCall = controller->isBusy();
    waitUntil([&] { return run.fired; });
    QObject::disconnect(busyConn);
    QObject::disconnect(doneConn);
    return run;
}

IoRun openAndWait(DocumentController *controller, const QString &path)
{
    return runIo(controller, &DocumentController::fileOpened, [=] { controller->openFile(path); });
}

IoRun saveAndWait(DocumentController *controller, const QString &path)
{
    return runIo(controller, &DocumentController::fileSaved, [=] { controller->saveFile(path); });
}

// " (error: ...)" for a check's label, if there was one.
QString errorNote(const IoRun &run)
{
    if (!run.fired)
        return QStringLiteral(" (no completion signal within %1 s)").arg(kIoTimeoutMs / 1000);
    return run.error.isEmpty() ? QString() : QStringLiteral(" (error: %1)").arg(run.error);
}

// The zip entry names in `path` ("" if not a zip).
QStringList zipEntries(const QString &path)
{
    QuaZip zip(path);
    return zip.open(QuaZip::mdUnzip) ? zip.getFileNameList() : QStringList();
}

// Calls a Main.qml root function, as the dialogs and shortcuts do.
void callRoot(QQuickWindow *window, const char *function, const QVariant &arg = {})
{
    if (arg.isValid())
        QMetaObject::invokeMethod(window, function, Q_ARG(QVariant, arg));
    else
        QMetaObject::invokeMethod(window, function);
}

bool dialogVisible(QQuickWindow *window, const QString &name)
{
    QObject *dialog = window->findChild<QObject *>(name);
    return dialog && dialog->property("visible").toBool();
}

void closeDialog(QQuickWindow *window, const QString &name)
{
    if (QObject *dialog = window->findChild<QObject *>(name))
        QMetaObject::invokeMethod(dialog, "close");
}

} // namespace

bool runAutoTest(QQuickWindow *window)
{
    Checker t;
    QTextStream(stdout) << "rune --auto-test" << Qt::endl;

    auto *controller = window->findChild<DocumentController *>(QStringLiteral("controller"));
    auto *editor = window->findChild<QQuickItem *>(QStringLiteral("editor"));
    auto *boldButton = window->findChild<QQuickItem *>(QStringLiteral("boldButton"));
    t.check(controller && editor && boldButton, "found controller, editor and Bold button");
    if (!t.ok())
        return false;

    auto *qmlDoc = editor->property("textDocument").value<QQuickTextDocument *>();
    QTextDocument *doc = controller->textDocument();
    t.check(qmlDoc && qmlDoc->textDocument() == doc, "TextEdit renders the controller-owned QTextDocument");

    controller->newDocument();
    t.check(doc->isEmpty() && !controller->isDirty(), "newDocument(): empty and clean");

    // Typing: real key events must land in the controller's document.
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Hello bold world"));
    t.check(doc->toPlainText() == QStringLiteral("Hello bold world"),
            QStringLiteral("typed text reached the document: \"%1\"").arg(doc->toPlainText()));
    t.check(controller->isDirty(), "document is dirty after typing");

    // Bold on a selection, via the same path the toolbar uses.
    QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 6), Q_ARG(int, 10));
    t.check(!controller->isBold(), "selection \"bold\" reports not bold before toggle");
    controller->toggleBold();
    t.check(controller->isBold(), "isBold() true after toggleBold()");
    t.check(boldButton->property("active").toBool(), "toolbar Bold button highlighted");
    t.check(formatOf(doc, 6, 10) == "B", QStringLiteral("\"bold\" is bold (%1)").arg(formatOf(doc, 6, 10)));
    t.check(formatOf(doc, 0, 6).isEmpty() && formatOf(doc, 10, 16).isEmpty(), "surrounding text unformatted");

    // Collapsed cursor at the end: italic arms a pending format for typing.
    editor->setProperty("cursorPosition", doc->characterCount() - 1);
    t.check(!boldButton->property("active").toBool(), "Bold button off after moving cursor to plain text");
    controller->toggleItalic();
    t.check(controller->isItalic(), "isItalic() true for pending format at collapsed cursor");
    typeText(window, QStringLiteral(" tail"));
    t.check(formatOf(doc, 16, 21) == "I", QStringLiteral("text typed after toggleItalic() is italic (%1)").arg(formatOf(doc, 16, 21)));
    t.check(formatOf(doc, 11, 16).isEmpty(), "\"world\" not italic");

    // Collapsed cursor inside a word: underline applies to the word.
    editor->setProperty("cursorPosition", 2);
    controller->toggleUnderline();
    t.check(formatOf(doc, 0, 5) == "U", QStringLiteral("cursor inside \"Hello\" underlines the word (%1)").arg(formatOf(doc, 0, 5)));
    controller->toggleUnderline();
    t.check(formatOf(doc, 0, 5).isEmpty(), "second toggle removes the underline");

    const bool saved = controller->saveToOdf(kStage1Path);
    t.check(saved, "saveToOdf(" + kStage1Path + ")");
    t.check(!controller->isDirty(), "clean after save");
    t.check(controller->currentPath() == kStage1Path, "currentPath = " + controller->currentPath());
    QTextStream(stdout) << "  final text: \"" << doc->toPlainText() << "\"" << Qt::endl;

    // Export sanitizer: list directly followed by a table (the native-spike
    // bug). Exercised on a scratch controller; Stage 1 has no table UI.
    DocumentController scratch;
    QTextDocument *sdoc = scratch.textDocument();
    sdoc->setHtml(QStringLiteral("<ul><li>Alpha</li><li>Beta</li></ul>"
                                 "<table border=\"1\"><tr><td>Cell A</td><td>Cell B</td></tr></table>"
                                 "<p>After</p>"));
    const QTextBlock before = blockBeforeFirstTable(sdoc);
    t.check(before.isValid() && before.textList(), "scratch doc: list block directly precedes table");
    const int blocksBefore = sdoc->blockCount();
    t.check(scratch.saveToOdf(kSanitizePath), "saveToOdf(" + kSanitizePath + ")");
    t.check(sdoc->blockCount() == blocksBefore, "save left the live document unchanged");

    std::unique_ptr<QTextDocument> copy(sdoc->clone());
    DocumentController::sanitizeForOdfExport(copy.get());
    const QTextBlock separator = blockBeforeFirstTable(copy.get());
    t.check(separator.isValid() && !separator.textList() && separator.text().isEmpty(),
            "sanitized copy: empty non-list paragraph between list and table");
    t.check(separator.isValid() && separator.previous().textList(), "sanitized copy: list still ends right before it");

    // --- Stage 2: alignment, lists, font, color ---
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Centered paragraph"));
    editor->setProperty("cursorPosition", 3);
    controller->setAlignment(Qt::AlignHCenter);
    t.check(controller->currentAlignment() == int(Qt::AlignHCenter), "currentAlignment() reports center after setAlignment");
    // Repeater-generated children aren't reliably found via findChild; check
    // the bound property the toolbar buttons read from instead.
    t.check(window->property("alignment").toInt() == int(Qt::AlignHCenter), "toolbar alignment state reflects center");
    t.check(controller->saveToOdf(kStage2AlignPath), "saveToOdf(" + kStage2AlignPath + ") [alignment]");

    // Bullet list: 3 items, then demote the middle one.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Alpha"));
    controller->toggleBulletList();
    t.check(controller->isInBulletList(), "isInBulletList() true after toggleBulletList()");
    auto *bulletButton = window->findChild<QQuickItem *>(QStringLiteral("bulletButton"));
    t.check(bulletButton && bulletButton->property("active").toBool(), "toolbar bullet button highlighted");
    pressReturn(window);
    typeText(window, QStringLiteral("Beta"));
    pressReturn(window);
    typeText(window, QStringLiteral("Gamma"));
    t.check(doc->toPlainText() == QStringLiteral("Alpha\nBeta\nGamma"),
            QStringLiteral("3-item list text: \"%1\"").arg(doc->toPlainText()));
    t.check(controller->isInBulletList(), "still in bullet list after newlines");

    // Demote "Beta" (middle item) one level.
    const int betaPos = doc->toPlainText().indexOf(QStringLiteral("Beta")) + 1;
    editor->setProperty("cursorPosition", betaPos);
    QTextCursor betaCursor(doc);
    betaCursor.setPosition(betaPos);
    const int indentBefore = betaCursor.block().textList() ? betaCursor.block().textList()->format().indent() : -1;
    controller->demoteListItem();
    QTextCursor betaAfter(doc);
    betaAfter.setPosition(betaPos);
    const int indentAfter = betaAfter.block().textList() ? betaAfter.block().textList()->format().indent() : -1;
    t.check(indentAfter > indentBefore,
            QStringLiteral("demoteListItem() increased indent (%1 -> %2)").arg(indentBefore).arg(indentAfter));
    t.check(controller->saveToOdf(kStage2ListPath), "saveToOdf(" + kStage2ListPath + ") [list]");

    // Font family and size on a word.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Plain word"));
    QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 5));
    controller->setFontFamily(QStringLiteral("Serif"));
    t.check(controller->currentFontFamily() == QStringLiteral("Serif"),
            QStringLiteral("currentFontFamily() = \"%1\"").arg(controller->currentFontFamily()));
    controller->setFontSize(24);
    t.check(qFuzzyCompare(controller->currentFontSize(), 24.0),
            QStringLiteral("currentFontSize() = %1").arg(controller->currentFontSize()));
    t.check(controller->saveToOdf(kStage2FontPath), "saveToOdf(" + kStage2FontPath + ") [font]");

    // Text color: set, then clear back to auto.
    QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 6), Q_ARG(int, 10));
    controller->setTextColor(QStringLiteral("#cc0000"));
    t.check(controller->currentTextColor() == QStringLiteral("#cc0000"),
            QStringLiteral("currentTextColor() = \"%1\"").arg(controller->currentTextColor()));
    controller->setTextColor(QStringLiteral("auto"));
    t.check(controller->currentTextColor() == QStringLiteral("auto"),
            QStringLiteral("currentTextColor() after clearing = \"%1\"").arg(controller->currentTextColor()));
    // Re-apply color for the saved file so the export check below has something to find.
    controller->setTextColor(QStringLiteral("#cc0000"));
    t.check(controller->saveToOdf(kStage2ColorPath), "saveToOdf(" + kStage2ColorPath + ") [color]");

    // --- Stage 3: tables ---

    // The Stage 1 sanitizer bug through the production path: a bullet list
    // typed through the UI, then insertTable() straight after its last item
    // with no paragraph in between.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Alpha"));
    controller->toggleBulletList();
    pressReturn(window);
    typeText(window, QStringLiteral("Beta"));
    t.check(doc->toPlainText() == QStringLiteral("Alpha\nBeta") && controller->isInBulletList(),
            QStringLiteral("2-item bullet list typed: \"%1\"").arg(doc->toPlainText()));
    controller->insertTable(2, 2);
    const QTextBlock listEnd = blockBeforeFirstTable(doc);
    t.check(listEnd.isValid() && listEnd.textList() && listEnd.text() == QStringLiteral("Beta"),
            "live doc: list item \"Beta\" directly precedes the inserted table (the bug pattern)");
    t.check(controller->isInTable() && cellAtCursor(doc, editor) == QStringLiteral("0,0"),
            "insertTable() puts the cursor in cell 0,0: " + cellAtCursor(doc, editor));
    typeText(window, QStringLiteral("Cell A"));
    pressTab(window);
    typeText(window, QStringLiteral("Cell B"));
    pressTab(window);
    typeText(window, QStringLiteral("Cell C"));
    pressTab(window);
    typeText(window, QStringLiteral("Cell D"));
    const int listTableBlocks = doc->blockCount();
    t.check(controller->saveToOdf(kStage3ListTablePath), "saveToOdf(" + kStage3ListTablePath + ") [list then table]");
    t.check(doc->blockCount() == listTableBlocks && blockBeforeFirstTable(doc).textList(),
            "save left the live list-then-table document unchanged");
    const QString listTableHtml = convertWithSoffice(kStage3ListTablePath);
    logConversion(kStage3ListTablePath, listTableHtml);
    t.check(countOf(listTableHtml, QStringLiteral("table")) == 1 && countOf(listTableHtml, QStringLiteral("td")) == 4
                && countOf(listTableHtml, QStringLiteral("li")) == 2,
            "soffice: list-then-table export has the list (2 <li>) and the table (4 <td>)");
    t.check(textOf(listTableHtml).contains(QStringLiteral("Alpha Beta Cell A Cell B Cell C Cell D")),
            "soffice: list items and all four cells present, in order");
    // Control: the same document written without the sanitizer loses the
    // table, so the check above really went through the bug.
    {
        QTextDocumentWriter raw(kStage3ListTableRawPath, QByteArrayLiteral("ODF"));
        raw.write(doc);
    }
    const QString rawHtml = convertWithSoffice(kStage3ListTableRawPath);
    logConversion(kStage3ListTableRawPath, rawHtml);
    t.check(!rawHtml.isEmpty() && countOf(rawHtml, QStringLiteral("table")) == 0
                && !textOf(rawHtml).contains(QStringLiteral("Cell A")),
            "control: the unsanitized export of the same document loses the table in soffice");

    // Table button, isInTable() and the row/column buttons' availability.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Intro"));
    auto *tableButton = window->findChild<QQuickItem *>(QStringLiteral("tableButton"));
    auto *insertRowButton = window->findChild<QQuickItem *>(QStringLiteral("insertRowButton"));
    auto *deleteRowButton = window->findChild<QQuickItem *>(QStringLiteral("deleteRowButton"));
    auto *insertColumnButton = window->findChild<QQuickItem *>(QStringLiteral("insertColumnButton"));
    auto *deleteColumnButton = window->findChild<QQuickItem *>(QStringLiteral("deleteColumnButton"));
    t.check(tableButton && insertRowButton && deleteRowButton && insertColumnButton && deleteColumnButton,
            "found table toolbar buttons");
    if (!t.ok())
        return false;
    const auto rowColumnAvailable = [&] {
        return insertRowButton->property("available").toBool() && deleteRowButton->property("available").toBool()
            && insertColumnButton->property("available").toBool() && deleteColumnButton->property("available").toBool();
    };
    t.check(!controller->isInTable() && !insertRowButton->property("available").toBool(),
            "outside a table: isInTable() false, row/column buttons unavailable");
    QMetaObject::invokeMethod(tableButton, "clicked");
    QTextTable *table = tableAtCursor(doc, editor);
    t.check(table && table->rows() == 3 && table->columns() == 3,
            QStringLiteral("Table button inserted a 3x3 table (%1x%2)")
                .arg(table ? table->rows() : 0).arg(table ? table->columns() : 0));
    if (!table)
        return false;
    t.check(controller->isInTable() && rowColumnAvailable(), "in a cell: isInTable() true, row/column buttons available");
    // Fill row by row ("r1c2" = row 1, column 2) so row/column edits show up by content.
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            typeText(window, QStringLiteral("r%1c%2").arg(r).arg(c));
            if (r < 2 || c < 2)
                pressTab(window);
        }
    editor->setProperty("cursorPosition", doc->characterCount() - 1);
    typeText(window, QStringLiteral("Trailing"));
    t.check(!controller->isInTable() && !rowColumnAvailable(),
            "in trailing text after the table: isInTable() false, row/column buttons unavailable");

    // +Row / +Col from cell 1,1: one more of each, after the cursor's.
    editor->setProperty("cursorPosition", table->cellAt(1, 1).firstCursorPosition().position());
    QMetaObject::invokeMethod(insertRowButton, "clicked");
    t.check(table->rows() == 4 && table->cellAt(2, 0).firstCursorPosition().block().text().isEmpty()
                && table->cellAt(3, 0).firstCursorPosition().block().text() == QStringLiteral("r2c0"),
            QStringLiteral("+Row: 3 -> %1 rows, empty row inserted below row 1").arg(table->rows()));
    QMetaObject::invokeMethod(insertColumnButton, "clicked");
    t.check(table->columns() == 4 && table->cellAt(0, 2).firstCursorPosition().block().text().isEmpty()
                && table->cellAt(0, 3).firstCursorPosition().block().text() == QStringLiteral("r0c2"),
            QStringLiteral("+Col: 3 -> %1 columns, empty column inserted right of column 1").arg(table->columns()));
    t.check(controller->saveToOdf(kStage3TablePath), "saveToOdf(" + kStage3TablePath + ") [4x4]");
    const QString grownHtml = convertWithSoffice(kStage3TablePath);
    logConversion(kStage3TablePath, grownHtml);
    t.check(countOf(grownHtml, QStringLiteral("tr")) == 4 && countOf(grownHtml, QStringLiteral("td")) == 16,
            "soffice: grown table has 4 rows, 16 cells");

    // −Row / −Col at cell 1,1: removes row 1 ("r1c*") and then column 1 ("*c1").
    t.check(cellAtCursor(doc, editor) == QStringLiteral("1,1"), "cursor still in cell 1,1: " + cellAtCursor(doc, editor));
    QMetaObject::invokeMethod(deleteRowButton, "clicked");
    t.check(table->rows() == 3 && controller->isInTable(),
            QStringLiteral("−Row: 4 -> %1 rows, cursor still in the table").arg(table->rows()));
    QMetaObject::invokeMethod(deleteColumnButton, "clicked");
    t.check(table->columns() == 3 && controller->isInTable(),
            QStringLiteral("−Col: 4 -> %1 columns, cursor still in the table").arg(table->columns()));
    t.check(controller->saveToOdf(kStage3TablePath), "saveToOdf(" + kStage3TablePath + ") [after deletes]");
    const QString shrunkHtml = convertWithSoffice(kStage3TablePath);
    logConversion(kStage3TablePath, shrunkHtml);
    const QString shrunkText = textOf(shrunkHtml);
    t.check(countOf(shrunkHtml, QStringLiteral("tr")) == 3 && countOf(shrunkHtml, QStringLiteral("td")) == 9,
            "soffice: shrunk table has 3 rows, 9 cells");
    t.check(shrunkText.contains(QStringLiteral("Intro r0c0 r0c2 r2c0 r2c2 Trailing")) && !shrunkText.contains(QStringLiteral("r1c"))
                && !shrunkText.contains(QStringLiteral("c1")),
            "soffice: row 1 and column 1 content gone, the rest intact");

    // Bounds: sizes clamp to 1..50; the last row/column is never deleted.
    controller->newDocument();
    editor->forceActiveFocus();
    controller->insertTable(0, 99);
    table = tableAtCursor(doc, editor);
    t.check(table && table->rows() == 1 && table->columns() == 50,
            QStringLiteral("insertTable(0, 99) clamps to 1x50 (%1x%2)")
                .arg(table ? table->rows() : 0).arg(table ? table->columns() : 0));
    controller->deleteTableColumn();
    controller->insertTable(1, 1); // nested in the cell, then removed down to nothing
    controller->deleteTableRow();
    controller->deleteTableColumn();
    QTextTable *inner = tableAtCursor(doc, editor);
    t.check(inner && inner != table && inner->rows() == 1 && inner->columns() == 1,
            "deleting the only row/column of a 1x1 table leaves it intact");
    t.check(table && table->columns() == 49, "−Col on a 1x50 table leaves 49 columns");
    editor->setProperty("cursorPosition", 0); // the empty paragraph before the table
    controller->insertTableRow();
    controller->deleteTableRow();
    t.check(!controller->isInTable() && table && table->rows() == 1, "row commands outside a table are no-ops");

    // Tab / Shift+Tab move between cells in reading order.
    controller->newDocument();
    editor->forceActiveFocus();
    controller->insertTable(2, 2);
    table = tableAtCursor(doc, editor);
    QStringList visited{cellAtCursor(doc, editor)};
    const QStringList words{QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("third"),
                            QStringLiteral("fourth")};
    for (int i = 0; i < words.size(); ++i) {
        typeText(window, words[i]);
        if (i + 1 < words.size()) {
            pressTab(window);
            visited << cellAtCursor(doc, editor);
        }
    }
    t.check(visited == QStringList{"0,0", "0,1", "1,0", "1,1"}, "Tab visits cells " + visited.join(" -> "));
    t.check(!doc->toPlainText().contains('\t'), "Tab in a table inserted no tab characters");
    QStringList back;
    for (int i = 0; i < 4; ++i) {
        pressBacktab(window);
        back << cellAtCursor(doc, editor);
    }
    t.check(back == QStringList{"1,0", "0,1", "0,0", "0,0"},
            "Shift+Tab visits cells " + back.join(" -> ") + " (stops at the first)");
    t.check(controller->saveToOdf(kStage3TabPath), "saveToOdf(" + kStage3TabPath + ") [Tab fill]");
    const QString tabHtml = convertWithSoffice(kStage3TabPath);
    logConversion(kStage3TabPath, tabHtml);
    t.check(countOf(tabHtml, QStringLiteral("td")) == 4 && textOf(tabHtml).contains(QStringLiteral("first second third fourth")),
            "soffice: cells filled via Tab read \"first second third fourth\"");
    editor->setProperty("cursorPosition", table->cellAt(1, 1).lastCursorPosition().position());
    pressTab(window);
    t.check(table->rows() == 3 && cellAtCursor(doc, editor) == QStringLiteral("2,0"),
            "Tab in the last cell appends a row and moves into it: " + cellAtCursor(doc, editor));

    // Tab outside a table keeps its Stage 2 behaviour.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Item"));
    controller->toggleBulletList();
    pressReturn(window);
    typeText(window, QStringLiteral("Sub"));
    pressTab(window);
    QTextCursor subCursor(doc);
    subCursor.setPosition(editor->property("cursorPosition").toInt());
    t.check(listIndent(subCursor.block()) == 2 && !doc->toPlainText().contains('\t'),
            QStringLiteral("Tab in a list (no table) still demotes: indent %1").arg(listIndent(subCursor.block())));
    pressBacktab(window);
    t.check(listIndent(subCursor.block()) == 1, QStringLiteral("Shift+Tab in a list still promotes: indent %1")
                                                    .arg(listIndent(subCursor.block())));
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("x"));
    pressTab(window);
    typeText(window, QStringLiteral("y"));
    t.check(doc->toPlainText() == QStringLiteral("x\ty"),
            QStringLiteral("Tab in plain text still inserts a tab: \"%1\"").arg(doc->toPlainText()));

    // --- Stage 4a: reading ODF ---

    // Round trip: write with the UI, read back into a fresh controller.
    // Tab and the double space go through <text:tab>/<text:s>; for the
    // leading spaces Qt's writer also puts a newline and indent inside the
    // span before the <text:s>, which the reader must drop.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Title"));
    pressReturn(window);
    typeText(window, QStringLiteral("Hello bold italic under"));
    pressTab(window);
    typeText(window, QStringLiteral("two  spaces"));
    pressReturn(window);
    typeText(window, QStringLiteral("  indented"));
    const auto select = [&](const QString &word) {
        const int start = positionOf(doc, word);
        QMetaObject::invokeMethod(editor, "select", Q_ARG(int, start), Q_ARG(int, int(start + word.size())));
    };
    select(QStringLiteral("Title"));
    controller->toggleBold();
    select(QStringLiteral("bold"));
    controller->toggleBold();
    select(QStringLiteral("italic"));
    controller->toggleItalic();
    select(QStringLiteral("under"));
    controller->toggleUnderline();
    const QString written = doc->toPlainText();
    t.check(controller->saveToOdf(kStage4RoundTripPath), "saveToOdf(" + kStage4RoundTripPath + ") [round trip]");

    DocumentController reader;
    QTextDocument *rdoc = reader.textDocument();
    int pathSignals = 0;
    QObject::connect(&reader, &DocumentController::currentPathChanged, [&] { ++pathSignals; });
    const bool opened = reader.openOdf(kStage4RoundTripPath);
    t.check(opened, "openOdf(" + kStage4RoundTripPath + ") on a fresh controller");
    QTextStream(stdout) << "  read back blocks: " << blocksOf(rdoc) << Qt::endl;
    t.check(rdoc->toPlainText() == written,
            QStringLiteral("round trip: plain text matches what was written (%1 blocks)").arg(rdoc->blockCount()));
    const auto formatOfWord = [](QTextDocument *d, const QString &word) {
        const int start = positionOf(d, word);
        return start < 0 ? QStringLiteral("missing") : formatOf(d, start, int(start + word.size()));
    };
    const QString titleFmt = formatOfWord(rdoc, QStringLiteral("Title"));
    const QString helloFmt = formatOfWord(rdoc, QStringLiteral("Hello"));
    const QString boldFmt = formatOfWord(rdoc, QStringLiteral("bold"));
    const QString italicFmt = formatOfWord(rdoc, QStringLiteral("italic"));
    const QString underFmt = formatOfWord(rdoc, QStringLiteral("under"));
    const QString spacesFmt = formatOfWord(rdoc, QStringLiteral("two  spaces"));
    QTextStream(stdout) << "  read back formats: Title=" << titleFmt << " Hello=" << helloFmt << " bold=" << boldFmt
                        << " italic=" << italicFmt << " under=" << underFmt << " \"two  spaces\"=" << spacesFmt
                        << Qt::endl;
    t.check(titleFmt == "B" && boldFmt == "B" && italicFmt == "I" && underFmt == "U",
            "round trip: Title/bold/italic/under read back as B/B/I/U");
    t.check(helloFmt.isEmpty() && spacesFmt.isEmpty(),
            "round trip: unformatted text reads back plain");
    t.check(!reader.isDirty() && reader.currentPath() == kStage4RoundTripPath && pathSignals == 1,
            "round trip: clean, currentPath set, currentPathChanged emitted once");

    // Same file through the window's controller, so the TextEdit shows it.
    controller->newDocument();
    t.check(controller->openOdf(kStage4RoundTripPath) && doc->toPlainText() == written
                && qmlDoc->textDocument() == doc && !controller->isDirty(),
            "openOdf() on the window's controller: TextEdit shows the read document, clean");
    t.check(!doc->isUndoAvailable(), "opening leaves no undo history (can't undo back to the old document)");

    // A file this code did not write: HTML fixture -> .odt by LibreOffice.
    QDir().mkpath(kStage4ExternalDir);
    const QString fixturePath = kStage4ExternalDir + QStringLiteral("/fixture.html");
    {
        QFile fixture(fixturePath);
        t.check(fixture.open(QIODevice::WriteOnly | QIODevice::Truncate), "wrote HTML fixture " + fixturePath);
        fixture.write("<html><body><h1>External Heading</h1>"
                      "<p>Plain <b>bold</b> <i>italic</i> <u>under</u> end.</p>"
                      "<h2>Second level</h2>"
                      "<ul><li>Item one</li><li>Item two</li></ul>"
                      "<p>Last paragraph.</p></body></html>");
    }
    const QString externalPath = sofficeHtmlToOdt(fixturePath, kStage4ExternalDir + QStringLiteral("/out"));
    t.check(!externalPath.isEmpty(), "soffice converted the HTML fixture to " + externalPath);
    DocumentController external;
    QTextDocument *edoc = external.textDocument();
    t.check(external.openOdf(externalPath), "openOdf() of the LibreOffice-written file");
    QTextStream(stdout) << "  external blocks: " << blocksOf(edoc) << Qt::endl;
    const QString expectedExternal = QStringLiteral(
        "External Heading\nPlain bold italic under end.\nSecond level\nItem one\nItem two\nLast paragraph.");
    t.check(edoc->toPlainText() == expectedExternal, "external: plain text, paragraph per block, in order");
    const QString extBold = formatOfWord(edoc, QStringLiteral("bold"));
    const QString extItalic = formatOfWord(edoc, QStringLiteral("italic"));
    const QString extUnder = formatOfWord(edoc, QStringLiteral("under"));
    const QString extPlain = formatOfWord(edoc, QStringLiteral("Plain"));
    QTextStream(stdout) << "  external formats: Plain=" << extPlain << " bold=" << extBold << " italic=" << extItalic
                        << " under=" << extUnder << Qt::endl;
    t.check(extBold == "B" && extItalic == "I" && extUnder == "U" && extPlain.isEmpty(),
            "external: LibreOffice's T1/T2/T3 span styles read as B/I/U");
    const QTextBlock second = blockWithText(edoc, QStringLiteral("Second level"));
    t.check(second.isValid() && second.blockFormat().headingLevel() == 2
                && formatOf(edoc, second.position(), second.position() + second.length() - 1) == "B",
            QStringLiteral("external: <text:h text:outline-level=\"2\"> read as a bold heading, level %1")
                .arg(second.isValid() ? second.blockFormat().headingLevel() : -1));

    // --- Stage 4b: lists, tables, font/color/alignment, styles.xml ---

    // Round trip: bullet list, one item demoted.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Alpha"));
    controller->toggleBulletList();
    pressReturn(window);
    typeText(window, QStringLiteral("Beta"));
    QTextCursor betaCur(doc);
    betaCur.setPosition(doc->characterCount() - 1);
    editor->setProperty("cursorPosition", betaCur.position());
    controller->demoteListItem();
    pressReturn(window);
    typeText(window, QStringLiteral("Gamma"));
    const QString listWritten = doc->toPlainText();
    t.check(controller->saveToOdf(kStage4bListPath), "saveToOdf(" + kStage4bListPath + ") [list]");
    DocumentController listReader;
    QTextDocument *ldoc = listReader.textDocument();
    const bool listReadOk = listReader.openOdf(kStage4bListPath);
    const QString listReadBack = ldoc->toPlainText();
    t.check(listReadOk && listReadBack == listWritten,
            QStringLiteral("round trip: list text matches: \"%1\"").arg(QString(listReadBack).replace('\n', '|')));
    const QTextBlock alphaBlock = blockWithText(ldoc, QStringLiteral("Alpha"));
    const QTextBlock betaBlock = blockWithText(ldoc, QStringLiteral("Beta"));
    const QTextBlock gammaBlock = blockWithText(ldoc, QStringLiteral("Gamma"));
    t.check(alphaBlock.isValid() && alphaBlock.textList() && gammaBlock.isValid() && gammaBlock.textList(),
            "round trip: list items read back as list blocks");
    t.check(betaBlock.isValid() && listIndent(betaBlock) > listIndent(alphaBlock),
            QStringLiteral("round trip: demoted item's indent survived (%1 > %2)")
                .arg(listIndent(betaBlock)).arg(listIndent(alphaBlock)));

    // Round trip: 2x2 table with distinct cell text.
    controller->newDocument();
    editor->forceActiveFocus();
    controller->insertTable(2, 2);
    QTextTable *wtable = doc->rootFrame()->childFrames().isEmpty() ? nullptr
        : qobject_cast<QTextTable *>(doc->rootFrame()->childFrames().first());
    t.check(wtable != nullptr, "insertTable(2,2) produced a QTextTable");
    if (wtable) {
        const QStringList cellText = {"R0C0", "R0C1", "R1C0", "R1C1"};
        int i = 0;
        for (int r = 0; r < 2; ++r) {
            for (int c = 0; c < 2; ++c) {
                QTextCursor cc = wtable->cellAt(r, c).firstCursorPosition();
                cc.insertText(cellText[i++]);
            }
        }
    }
    t.check(controller->saveToOdf(kStage4bTablePath), "saveToOdf(" + kStage4bTablePath + ") [table]");
    DocumentController tableReader;
    QTextDocument *tdoc = tableReader.textDocument();
    t.check(tableReader.openOdf(kStage4bTablePath), "openOdf() [table round trip]");
    QTextTable *rtable = nullptr;
    for (auto it = tdoc->rootFrame()->begin(); !it.atEnd(); ++it) {
        if (auto *tbl = qobject_cast<QTextTable *>(it.currentFrame())) { rtable = tbl; break; }
    }
    t.check(rtable && rtable->rows() == 2 && rtable->columns() == 2,
            QStringLiteral("round trip: table read back as %1x%2")
                .arg(rtable ? rtable->rows() : -1).arg(rtable ? rtable->columns() : -1));
    if (rtable) {
        const QString r0c0 = rtable->cellAt(0, 0).firstCursorPosition().block().text();
        const QString r1c1 = rtable->cellAt(1, 1).firstCursorPosition().block().text();
        t.check(r0c0 == QStringLiteral("R0C0") && r1c1 == QStringLiteral("R1C1"),
                QStringLiteral("round trip: cell content in place (0,0)=\"%1\" (1,1)=\"%2\"").arg(r0c0, r1c1));
    }

    // Round trip: font, size, color, alignment on one word.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Styled word here"));
    QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 6));
    controller->setFontFamily(QStringLiteral("Serif"));
    controller->setFontSize(24);
    controller->setTextColor(QStringLiteral("#cc0000"));
    controller->setAlignment(Qt::AlignHCenter);
    t.check(controller->saveToOdf(kStage4bFontPath), "saveToOdf(" + kStage4bFontPath + ") [font/color/align]");
    DocumentController fontReader;
    QTextDocument *fdoc = fontReader.textDocument();
    t.check(fontReader.openOdf(kStage4bFontPath), "openOdf() [font round trip]");
    QTextCursor fcur(fdoc);
    fcur.setPosition(1);
    fcur.setPosition(6, QTextCursor::KeepAnchor);
    const QTextCharFormat gotFmt = fcur.charFormat();
    const QTextBlockFormat gotBlockFmt = fcur.blockFormat();
    const QString gotFamily = gotFmt.fontFamilies().toStringList().value(0);
    QTextStream(stdout) << "  font round trip: family=" << gotFamily << " size=" << gotFmt.fontPointSize()
                        << " color=" << gotFmt.foreground().color().name() << " align=" << int(gotBlockFmt.alignment())
                        << Qt::endl;
    t.check(gotFamily == QStringLiteral("Serif"), "round trip: font family survived");
    t.check(qFuzzyCompare(gotFmt.fontPointSize(), 24.0), "round trip: font size survived");
    t.check(gotFmt.foreground().color() == QColor("#cc0000"), "round trip: color survived");
    t.check((gotBlockFmt.alignment() & Qt::AlignHorizontal_Mask) == Qt::AlignHCenter, "round trip: alignment survived");

    // Named-style inheritance: a hand-built ODF with NO <text:h> at all --
    // heading-like formatting comes purely from a 3-level styles.xml chain
    // (MyHeading -> Heading_20_1 -> BaseSize). This is the exact gap Stage 4a
    // found and this stage exists to close.
    const QByteArray stylesXml = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<office:document-styles xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
        "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
        "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\">"
        "<office:styles>"
        "<style:style style:name=\"BaseSize\" style:family=\"paragraph\">"
        "<style:text-properties fo:font-size=\"20pt\"/></style:style>"
        "<style:style style:name=\"Heading_20_1\" style:family=\"paragraph\" style:parent-style-name=\"BaseSize\">"
        "<style:text-properties fo:font-weight=\"bold\"/></style:style>"
        "<style:style style:name=\"MyHeading\" style:family=\"paragraph\" style:parent-style-name=\"Heading_20_1\">"
        "<style:text-properties fo:color=\"#0000ff\"/></style:style>"
        "</office:styles></office:document-styles>");
    const QByteArray contentXml = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
        "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
        "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\">"
        "<office:body><office:text>"
        "<text:p text:style-name=\"MyHeading\">Styled Heading Text</text:p>"
        "</office:text></office:body></office:document-content>");
    t.check(writeZipMulti(kStage4bInheritPath, {{QStringLiteral("content.xml"), contentXml},
                                                 {QStringLiteral("styles.xml"), stylesXml}}),
            "wrote hand-built named-style-inheritance fixture");
    DocumentController inheritReader;
    QTextDocument *idoc = inheritReader.textDocument();
    QString inheritErr;
    const bool inheritOk = OdfReader::readInto(idoc, kStage4bInheritPath, &inheritErr);
    t.check(inheritOk, "openOdf() of hand-built 3-level inheritance fixture: " + inheritErr);
    if (inheritOk) {
        QTextCursor icur(idoc);
        icur.setPosition(0);
        icur.setPosition(idoc->characterCount() - 1, QTextCursor::KeepAnchor);
        const QTextCharFormat ifmt = icur.charFormat();
        QTextStream(stdout) << "  inheritance: size=" << ifmt.fontPointSize()
                            << " bold=" << (ifmt.fontWeight() >= QFont::Bold)
                            << " color=" << ifmt.foreground().color().name() << Qt::endl;
        t.check(qFuzzyCompare(ifmt.fontPointSize(), 20.0),
                "inheritance: font-size 20pt inherited from grandparent BaseSize");
        t.check(ifmt.fontWeight() >= QFont::Bold, "inheritance: bold inherited from parent Heading_20_1");
        t.check(ifmt.foreground().color() == QColor("#0000ff"), "inheritance: color set directly by MyHeading");
    }

    // Focused unit test of StyleSheet::resolve() itself, isolated from file I/O.
    {
        OdfReader::StyleSheet sheet;
        OdfReader::Style base, mid, leaf;
        base.blockFormat.setProperty(QTextFormat::FontPointSize, 20.0);
        QTextCharFormat baseChar; baseChar.setFontPointSize(20.0);
        base.charFormat = baseChar;
        sheet.insert(QStringLiteral("paragraph"), QStringLiteral("Base"), base);
        QTextCharFormat midChar; midChar.setFontWeight(QFont::Bold);
        mid.charFormat = midChar;
        mid.parent = QStringLiteral("Base");
        sheet.insert(QStringLiteral("paragraph"), QStringLiteral("Mid"), mid);
        QTextCharFormat leafChar; leafChar.setForeground(QColor("#0000ff"));
        leaf.charFormat = leafChar;
        leaf.parent = QStringLiteral("Mid");
        sheet.insert(QStringLiteral("paragraph"), QStringLiteral("Leaf"), leaf);
        const OdfReader::Style resolved = sheet.resolve(QStringLiteral("paragraph"), QStringLiteral("Leaf"));
        t.check(qFuzzyCompare(resolved.charFormat.fontPointSize(), 20.0)
                    && resolved.charFormat.fontWeight() >= QFont::Bold
                    && resolved.charFormat.foreground().color() == QColor("#0000ff"),
                "StyleSheet::resolve(): 3-level chain merges all ancestors correctly");
        // Cycle guard: Loop -> Loop (self-reference) must not hang or crash.
        OdfReader::Style loop;
        loop.parent = QStringLiteral("Loop");
        sheet.insert(QStringLiteral("paragraph"), QStringLiteral("Loop"), loop);
        const OdfReader::Style loopResolved = sheet.resolve(QStringLiteral("paragraph"), QStringLiteral("Loop"));
        Q_UNUSED(loopResolved);
        t.check(true, "StyleSheet::resolve(): self-referencing style does not hang");
    }

    // My Stage 4b checks above reused the shared controller/editor (needed for
    // real key-event typing); restore it to Stage 4a's round-trip document so
    // the "failures leave it alone" checks below are still testing what they
    // say they are.
    t.check(controller->openOdf(kStage4RoundTripPath) && doc->toPlainText() == written,
            "restored shared controller to the Stage 4a round-trip document");

    // Failures leave the open document alone.
    controller->setSelection(0, 0, 0);
    const auto unchanged = [&] {
        return doc->toPlainText() == written && controller->currentPath() == kStage4RoundTripPath
            && !controller->isDirty();
    };
    t.check(!controller->openOdf(kStage4MissingPath) && unchanged(),
            "openOdf(non-existent path) returns false, document and path unchanged");
    {
        QFile notZip(kStage4NotZipPath);
        t.check(notZip.open(QIODevice::WriteOnly | QIODevice::Truncate)
                    && notZip.write("This is plain text with an .odt extension.\n") > 0,
                "wrote " + kStage4NotZipPath);
    }
    t.check(!controller->openOdf(kStage4NotZipPath) && unchanged(),
            "openOdf(plain text named .odt) returns false, document unchanged");
    t.check(writeZip(kStage4NoContentPath, QStringLiteral("hello.txt"), "hi")
                && !controller->openOdf(kStage4NoContentPath) && unchanged(),
            "openOdf(zip without content.xml) returns false, document unchanged");
    t.check(writeZip(kStage4BadXmlPath, QStringLiteral("content.xml"),
                     "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\">"
                     "<office:body><office:text><p>truncated")
                && !controller->openOdf(kStage4BadXmlPath) && unchanged(),
            "openOdf(zip with truncated content.xml) returns false, document unchanged");

    // --- Stage 5: .docx via LibreOffice batch conversion ---

    const QStringList tempDirsBefore = bridgeTempDirs();

    // Round trip through .docx: heading-like bold title (there's no heading
    // UI), bold/italic words, a bullet list, then a table straight after
    // the list (the sanitizer's case), then a closing paragraph.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Quarterly Report"));
    pressReturn(window);
    typeText(window, QStringLiteral("Some bold and italic words"));
    pressReturn(window);
    typeText(window, QStringLiteral("First item"));
    controller->toggleBulletList();
    pressReturn(window);
    typeText(window, QStringLiteral("Second item"));
    controller->insertTable(2, 2);
    typeText(window, QStringLiteral("Cell A"));
    pressTab(window);
    typeText(window, QStringLiteral("Cell B"));
    pressTab(window);
    typeText(window, QStringLiteral("Cell C"));
    pressTab(window);
    typeText(window, QStringLiteral("Cell D"));
    editor->setProperty("cursorPosition", doc->characterCount() - 1);
    typeText(window, QStringLiteral("Closing line"));
    select(QStringLiteral("Quarterly Report"));
    controller->toggleBold();
    select(QStringLiteral("bold"));
    controller->toggleBold();
    select(QStringLiteral("italic"));
    controller->toggleItalic();
    const QString docxWritten = doc->toPlainText();
    // What actually goes into the file: the sanitizer adds an empty
    // paragraph between the list and the table.
    std::unique_ptr<QTextDocument> exported(doc->clone());
    DocumentController::sanitizeForOdfExport(exported.get());
    const QString docxExpected = exported->toPlainText();

    QStringList savePaths;
    const auto savePathConn = QObject::connect(controller, &DocumentController::currentPathChanged,
                                               [&] { savePaths << controller->currentPath(); });
    const bool dirtyBeforeSave = controller->isDirty();
    const IoRun docxSave = saveAndWait(controller, kStage5DocxPath);
    QObject::disconnect(savePathConn);
    t.check(dirtyBeforeSave && docxSave.fired && docxSave.success && docxSave.path == kStage5DocxPath,
            "saveFile(" + kStage5DocxPath + ") -> fileSaved(true)" + errorNote(docxSave));
    QTextStream(stdout) << "  saveFile(.docx): fileSaved after " << docxSave.elapsedMs << " ms" << Qt::endl;
    t.check(docxSave.busyAfterCall && !docxSave.completedInCall && !docxSave.busyAtSignal && !controller->isBusy()
                && docxSave.busyChanges == 2,
            QStringLiteral("saveFile(.docx): busy as the call returns, fileSaved later, busy false again by then "
                           "(busyChanged x%1)").arg(docxSave.busyChanges));
    t.check(!controller->isDirty() && controller->currentPath() == kStage5DocxPath
                && savePaths == QStringList{kStage5DocxPath},
            "saveFile(.docx): clean, currentPath is the .docx, currentPathChanged once with it (never the temp .odt): "
                + savePaths.join(QLatin1String(", ")));
    t.check(doc->toPlainText() == docxWritten, "saveFile(.docx) left the live document unchanged");
    const QStringList docxEntries = zipEntries(kStage5DocxPath);
    QTextStream(stdout) << "  saved .docx: " << QFileInfo(kStage5DocxPath).size() << " bytes, zip entries: "
                        << docxEntries.join(QLatin1String(", ")) << Qt::endl;
    t.check(docxEntries.contains(QStringLiteral("word/document.xml"))
                && docxEntries.contains(QStringLiteral("[Content_Types].xml")),
            "saved file is an OOXML package (word/document.xml, [Content_Types].xml)");

    // Independent check with real soffice, not DocxBridge.
    const QString savedText = sofficeToText(kStage5DocxPath, kStage5TextDir);
    QTextStream(stdout) << "  soffice " << QFileInfo(kStage5DocxPath).fileName() << " -> txt: \""
                        << QString(savedText).replace('\n', '|') << "\"" << Qt::endl;
    t.check(!savedText.isEmpty() && wordsOf(savedText) == wordsOf(docxWritten),
            "soffice: the saved .docx opens and its text is what was typed");

    DocumentController docxReader;
    QTextDocument *xdoc = docxReader.textDocument();
    QStringList openPaths;
    QObject::connect(&docxReader, &DocumentController::currentPathChanged,
                     [&] { openPaths << docxReader.currentPath(); });
    const IoRun docxOpen = openAndWait(&docxReader, kStage5DocxPath);
    t.check(docxOpen.fired && docxOpen.success && docxOpen.path == kStage5DocxPath,
            "openFile(" + kStage5DocxPath + ") on a fresh controller -> fileOpened(true)" + errorNote(docxOpen));
    QTextStream(stdout) << "  openFile(.docx): fileOpened after " << docxOpen.elapsedMs << " ms" << Qt::endl;
    t.check(docxOpen.busyAfterCall && !docxOpen.completedInCall && !docxOpen.busyAtSignal && !docxReader.isBusy()
                && docxOpen.busyChanges == 2,
            QStringLiteral("openFile(.docx): busy as the call returns, fileOpened later, busy false again by then "
                           "(busyChanged x%1)").arg(docxOpen.busyChanges));
    QTextStream(stdout) << "  .docx read back blocks: " << blocksOf(xdoc) << Qt::endl;
    t.check(xdoc->toPlainText() == docxExpected,
            QStringLiteral(".docx round trip: plain text matches what was written (%1 blocks)").arg(xdoc->blockCount()));
    const QString xTitle = formatOfWord(xdoc, QStringLiteral("Quarterly Report"));
    const QString xBold = formatOfWord(xdoc, QStringLiteral("bold"));
    const QString xItalic = formatOfWord(xdoc, QStringLiteral("italic"));
    const QString xSome = formatOfWord(xdoc, QStringLiteral("Some"));
    QTextStream(stdout) << "  .docx read back formats: Title=" << xTitle << " Some=" << xSome << " bold=" << xBold
                        << " italic=" << xItalic << Qt::endl;
    t.check(xTitle == "B" && xBold == "B" && xItalic == "I" && xSome.isEmpty(),
            ".docx round trip: title/bold/italic read back as B/B/I, plain text plain");
    const QTextBlock xFirst = blockWithText(xdoc, QStringLiteral("First item"));
    const QTextBlock xSecond = blockWithText(xdoc, QStringLiteral("Second item"));
    t.check(xFirst.isValid() && xFirst.textList() && xSecond.isValid() && xSecond.textList() == xFirst.textList()
                && listKindIsBullet(xFirst.textList()),
            ".docx round trip: both items read back in one bullet list");
    QTextTable *xTable = nullptr;
    for (auto it = xdoc->rootFrame()->begin(); !it.atEnd() && !xTable; ++it)
        xTable = qobject_cast<QTextTable *>(it.currentFrame());
    t.check(xTable && xTable->rows() == 2 && xTable->columns() == 2
                && xTable->cellAt(1, 1).firstCursorPosition().block().text() == QStringLiteral("Cell D"),
            QStringLiteral(".docx round trip: 2x2 table, cell (1,1) = \"Cell D\" (%1x%2)")
                .arg(xTable ? xTable->rows() : -1).arg(xTable ? xTable->columns() : -1));
    t.check(!docxReader.isDirty() && docxReader.currentPath() == kStage5DocxPath
                && openPaths == QStringList{kStage5DocxPath},
            "openFile(.docx): clean, currentPath is the .docx, currentPathChanged once with it: "
                + openPaths.join(QLatin1String(", ")));

    // A real external .docx this pipeline didn't write.
    DocumentController sampleReader;
    QTextDocument *sdocx = sampleReader.textDocument();
    const IoRun sampleOpen = QFileInfo::exists(kSampleDocx) ? openAndWait(&sampleReader, kSampleDocx) : IoRun();
    t.check(sampleOpen.success, "openFile(" + kSampleDocx + ")" + errorNote(sampleOpen));
    QTextStream(stdout) << "  sample .docx blocks: " << blocksOf(sdocx) << Qt::endl;
    const QString sampleText = sofficeToText(kSampleDocx, kStage5TextDir);
    QTextStream(stdout) << "  soffice test.docx -> txt: \"" << QString(sampleText).replace('\n', '|') << "\"" << Qt::endl;
    t.check(sdocx->toPlainText().startsWith(QStringLiteral("Rune Spike Test Document")) && sdocx->blockCount() > 5,
            QStringLiteral("sample .docx: non-empty, starts with its title (%1 blocks)").arg(sdocx->blockCount()));
    t.check(!sampleText.isEmpty() && wordsOf(sdocx->toPlainText()) == wordsOf(sampleText),
            "sample .docx: our text matches soffice's own text export word for word");
    const QTextBlock sampleItem = blockWithText(sdocx, QStringLiteral("Render page 0 with paintTile"));
    QTextTable *sampleTable = nullptr;
    for (auto it = sdocx->rootFrame()->begin(); !it.atEnd() && !sampleTable; ++it)
        sampleTable = qobject_cast<QTextTable *>(it.currentFrame());
    t.check(sampleItem.isValid() && sampleItem.textList() && sampleTable && sampleTable->rows() == 3
                && sampleTable->columns() == 2,
            QStringLiteral("sample .docx: checklist read as a list, table as %1x%2")
                .arg(sampleTable ? sampleTable->rows() : -1).arg(sampleTable ? sampleTable->columns() : -1));
    DocumentController docReader;
    const bool docOpened = QFileInfo::exists(kSampleDoc) && openAndWait(&docReader, kSampleDoc).success;
    const QString docText = sofficeToText(kSampleDoc, kStage5TextDir);
    QTextStream(stdout) << "  sample .doc blocks: " << blocksOf(docReader.textDocument()) << Qt::endl;
    t.check(docOpened && !docText.isEmpty() && wordsOf(docReader.textDocument()->toPlainText()) == wordsOf(docText),
            QStringLiteral("openFile(%1) [legacy .doc]: %2 blocks, text matches soffice's export")
                .arg(kSampleDoc).arg(docReader.textDocument()->blockCount()));

    // Failures leave the open document alone, as for openOdf().
    t.check(controller->openOdf(kStage4RoundTripPath) && unchanged(),
            "restored shared controller to the Stage 4a round-trip document");
    const IoRun missingOpen = openAndWait(controller, kStage5MissingPath);
    t.check(missingOpen.fired && !missingOpen.success && !missingOpen.error.isEmpty() && unchanged(),
            "openFile(non-existent .docx) -> fileOpened(false), document and path unchanged: \"" + missingOpen.error + "\"");
    {
        QFile txt(kStage5TxtPath);
        QFile noExt(kStage5NoExtPath);
        t.check(txt.open(QIODevice::WriteOnly | QIODevice::Truncate) && txt.write("plain text\n") > 0
                    && noExt.open(QIODevice::WriteOnly | QIODevice::Truncate) && noExt.write("plain text\n") > 0,
                "wrote " + kStage5TxtPath + " and " + kStage5NoExtPath);
    }
    const IoRun txtOpen = openAndWait(controller, kStage5TxtPath);
    t.check(txtOpen.fired && !txtOpen.success && txtOpen.completedInCall && txtOpen.busyChanges == 0 && unchanged(),
            "openFile(existing .txt) -> fileOpened(false) at once (unsupported type), document unchanged: \""
                + txtOpen.error + "\"");
    const IoRun noExtOpen = openAndWait(controller, kStage5NoExtPath);
    t.check(noExtOpen.fired && !noExtOpen.success && noExtOpen.completedInCall && unchanged(),
            "openFile(existing file, no extension) -> fileOpened(false) at once, document unchanged");
    {
        QFile corrupt(kStage5CorruptPath);
        t.check(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate) && corrupt.write("PK\x03\x04garbage") > 0,
                "wrote truncated-zip " + kStage5CorruptPath);
    }
    const IoRun corruptOpen = openAndWait(controller, kStage5CorruptPath);
    t.check(corruptOpen.fired && !corruptOpen.success && !controller->isBusy() && unchanged(),
            "openFile(corrupt .docx, soffice exits non-zero) -> fileOpened(false), not busy, document unchanged");
    QString bridgeError;
    const bool corruptFailed = DocxBridge::convertToOdt(kStage5CorruptPath, &bridgeError).isEmpty();
    t.check(corruptFailed && bridgeError.contains(QStringLiteral("exit")),
            "DocxBridge::convertToOdt(corrupt): \"" + bridgeError + "\"");
    {
        // soffice not installed: an empty PATH.
        const QByteArray path = qgetenv("PATH");
        qputenv("PATH", "/nonexistent");
        bridgeError.clear();
        const bool notFound = DocxBridge::convertToOdt(kSampleDocx, &bridgeError).isEmpty();
        const IoRun noSoffice = openAndWait(controller, kSampleDocx);
        const bool openFailed = noSoffice.fired && !noSoffice.success;
        qputenv("PATH", path);
        t.check(notFound && bridgeError.contains(QStringLiteral("not found")) && openFailed && unchanged(),
                "soffice not in PATH: conversion and openFile() fail cleanly: \"" + bridgeError + "\"");
    }

    // Save failures: unsupported type writes nothing; a failed .docx
    // conversion/write leaves the document dirty and its path unchanged.
    QFile::remove(kStage5TxtPath);
    const IoRun txtSave = saveAndWait(controller, kStage5TxtPath);
    t.check(txtSave.fired && !txtSave.success && txtSave.completedInCall && !QFileInfo::exists(kStage5TxtPath)
                && unchanged(),
            "saveFile(.txt) -> fileSaved(false) at once, writes nothing, document unchanged");
    {
        DocumentController dirtyDoc;
        dirtyDoc.textDocument()->setPlainText(QStringLiteral("unsaved"));
        dirtyDoc.textDocument()->setModified(true);
        int signalsSeen = 0;
        QObject::connect(&dirtyDoc, &DocumentController::currentPathChanged, [&] { ++signalsSeen; });
        QObject::connect(&dirtyDoc, &DocumentController::dirtyChanged, [&] { ++signalsSeen; });
        bool stateDuringSave = false;
        const IoRun unwritable = runIo(&dirtyDoc, &DocumentController::fileSaved, [&] {
            dirtyDoc.saveFile(kStage5UnwritablePath);
            // The temporary .odt is written by now; the state must not show it.
            stateDuringSave = dirtyDoc.isBusy() && dirtyDoc.isDirty() && dirtyDoc.currentPath().isEmpty();
        });
        t.check(stateDuringSave, "saveFile(.docx) in flight: still dirty, no path (temporary .odt state not visible)");
        t.check(unwritable.fired && !unwritable.success && !dirtyDoc.isBusy() && dirtyDoc.isDirty()
                    && dirtyDoc.currentPath().isEmpty() && signalsSeen == 0,
                "saveFile(.docx in a missing directory) -> fileSaved(false), still dirty, no path, no "
                "dirty/path signals: \"" + unwritable.error + "\"");
    }

    // --- Asynchronous openFile()/saveFile() ---

    // .odt: in-process, done before the call returns, never busy.
    const IoRun odtSave = saveAndWait(controller, kAsyncOdtPath);
    t.check(odtSave.success && odtSave.completedInCall && !odtSave.busyAfterCall && odtSave.busyChanges == 0
                && controller->currentPath() == kAsyncOdtPath && !controller->isDirty(),
            QStringLiteral("saveFile(.odt): fileSaved(true) before the call returns, busy never set "
                           "(busyChanged x%1)").arg(odtSave.busyChanges) + errorNote(odtSave));
    const IoRun odtOpen = openAndWait(controller, kStage4RoundTripPath);
    t.check(odtOpen.success && odtOpen.completedInCall && !odtOpen.busyAfterCall && odtOpen.busyChanges == 0
                && unchanged(),
            QStringLiteral("openFile(.odt): fileOpened(true) before the call returns, busy never set "
                           "(busyChanged x%1)").arg(odtOpen.busyChanges) + errorNote(odtOpen));

    // Re-entrancy: a second call while busy fails at once and leaves the
    // first alone.
    {
        DocumentController busyDoc;
        QList<IoRun> opens;
        QList<IoRun> saves;
        QObject::connect(&busyDoc, &DocumentController::fileOpened, [&](bool ok, const QString &p, const QString &e) {
            IoRun r;
            r.fired = true;
            r.success = ok;
            r.path = p;
            r.error = e;
            r.busyAtSignal = busyDoc.isBusy();
            opens << r;
        });
        QObject::connect(&busyDoc, &DocumentController::fileSaved, [&](bool ok, const QString &p, const QString &e) {
            IoRun r;
            r.fired = true;
            r.success = ok;
            r.path = p;
            r.error = e;
            saves << r;
        });
        QFile::remove(kAsyncEditDocxPath);
        busyDoc.openFile(kSampleDocx);
        const bool firstBusy = busyDoc.isBusy() && opens.isEmpty();
        busyDoc.openFile(kStage5DocxPath);
        busyDoc.saveFile(kAsyncEditDocxPath);
        busyDoc.openFile(kAsyncOdtPath); // even the fast path
        const auto rejected = [](const IoRun &r) {
            return !r.success && r.error.contains(QStringLiteral("already in progress"));
        };
        t.check(firstBusy && opens.size() == 2 && saves.size() == 1 && rejected(opens[0])
                    && opens[0].path == kStage5DocxPath && rejected(opens[1]) && opens[1].path == kAsyncOdtPath
                    && rejected(saves[0]) && busyDoc.isBusy() && busyDoc.currentPath().isEmpty()
                    && busyDoc.textDocument()->isEmpty() && !QFileInfo::exists(kAsyncEditDocxPath),
                "while busy: second openFile(.docx), saveFile(.docx), openFile(.odt) each fail at once with \""
                    + (opens.isEmpty() ? QString() : opens[0].error)
                    + "\"; still busy, document, path and target file untouched");
        const bool firstDone = waitUntil([&] { return opens.size() == 3; });
        t.check(firstDone && opens.size() == 3 && opens[2].success && opens[2].path == kSampleDocx
                    && !opens[2].busyAtSignal && !busyDoc.isBusy()
                    && busyDoc.currentPath() == QFileInfo(kSampleDocx).absoluteFilePath()
                    && busyDoc.textDocument()->toPlainText().startsWith(QStringLiteral("Rune Spike Test Document")),
                "the first openFile(" + kSampleDocx + ") still completes: fileOpened(true), document loaded, not busy");
    }

    // The main thread is free while soffice converts. A 20 ms ticker plus
    // two single-shots must all run between openFile() returning and
    // fileOpened(); for contrast, the same conversion called synchronously
    // (as openFile() did before) lets no tick through.
    {
        QElapsedTimer clock;
        QStringList timeline;
        const auto mark = [&](const QString &what) { timeline << QStringLiteral("%1 ms %2").arg(clock.elapsed()).arg(what); };
        int ticks = 0;
        qint64 lastTick = 0;
        qint64 maxGap = 0;
        QTimer ticker;
        ticker.setInterval(20);
        QObject::connect(&ticker, &QTimer::timeout, [&] {
            const qint64 now = clock.elapsed();
            maxGap = std::max(maxGap, now - lastTick);
            lastTick = now;
            ++ticks;
        });

        // Synchronous baseline on this thread.
        clock.start();
        ticker.start();
        QString syncError;
        const QString syncOdt = DocxBridge::convertToOdt(kSampleDocx, &syncError);
        const qint64 syncMs = clock.elapsed();
        const int syncTicks = ticks;
        ticker.stop();
        DocxBridge::removeConvertedOdt(syncOdt);
        QTextStream(stdout) << "  synchronous convertToOdt() on the main thread: " << syncMs << " ms, ticks during it: "
                            << syncTicks << Qt::endl;
        t.check(!syncOdt.isEmpty() && syncTicks == 0,
                QStringLiteral("baseline: a synchronous conversion blocks the main thread (%1 ms, %2 ticks)")
                    .arg(syncMs).arg(syncTicks));

        DocumentController freeDoc;
        int ticksAtSignal = -1;
        bool opened = false;
        bool done = false;
        QObject::connect(&freeDoc, &DocumentController::fileOpened, [&](bool ok, const QString &, const QString &) {
            ticksAtSignal = ticks;
            opened = ok;
            done = true;
            mark(QStringLiteral("fileOpened(%1) [%2 ticks so far]").arg(ok ? "true" : "false").arg(ticks));
        });
        ticks = 0;
        maxGap = 0;
        clock.start();
        lastTick = 0;
        ticker.start();
        mark(QStringLiteral("openFile(test.docx) called"));
        freeDoc.openFile(kSampleDocx);
        mark(QStringLiteral("openFile() returned, busy=%1").arg(freeDoc.isBusy() ? "true" : "false"));
        QTimer::singleShot(0, &freeDoc, [&] { mark(QStringLiteral("singleShot(0) ran, busy=%1 [%2 ticks]")
                                             .arg(freeDoc.isBusy() ? "true" : "false").arg(ticks)); });
        QTimer::singleShot(250, &freeDoc, [&] { mark(QStringLiteral("singleShot(250) ran, busy=%1 [%2 ticks]")
                                               .arg(freeDoc.isBusy() ? "true" : "false").arg(ticks)); });
        waitUntil([&] { return done; });
        ticker.stop();
        QTextStream(stdout) << "  async openFile(.docx) timeline:" << Qt::endl;
        for (const QString &line : timeline)
            QTextStream(stdout) << "    " << line << Qt::endl;
        QTextStream(stdout) << "  ticks before fileOpened: " << ticksAtSignal << ", longest gap between ticks: "
                            << maxGap << " ms" << Qt::endl;
        const bool order = timeline.size() == 5 && timeline[1].contains(QStringLiteral("busy=true"))
            && timeline[2].contains(QStringLiteral("singleShot(0) ran, busy=true"))
            && timeline[3].contains(QStringLiteral("singleShot(250) ran, busy=true"))
            && timeline[4].contains(QStringLiteral("fileOpened(true)"));
        t.check(opened && order,
                "main thread free during conversion: singleShot(0) and singleShot(250) both ran while busy, "
                "before fileOpened");
        // Each tick is 20 ms; a conversion of 1 s or more must let dozens
        // through, and the event loop must never stall for long (the
        // follow-up openOdf() on this thread is the longest legitimate gap).
        t.check(ticksAtSignal >= 10 && maxGap < 500,
                QStringLiteral("20 ms ticker kept running during the conversion: %1 ticks, longest gap %2 ms")
                    .arg(ticksAtSignal).arg(maxGap));
    }

    // Edits made while a .docx save converts aren't in the file, so the
    // document must stay dirty.
    {
        DocumentController editDoc;
        QTextDocument *edoc = editDoc.textDocument();
        edoc->setPlainText(QStringLiteral("Snapshot text"));
        edoc->setModified(true);
        QFile::remove(kAsyncEditDocxPath);
        const IoRun editSave = runIo(&editDoc, &DocumentController::fileSaved, [&] {
            editDoc.saveFile(kAsyncEditDocxPath);
            QTextCursor(edoc).insertText(QStringLiteral("Typed during save "));
        });
        const QString editSavedText = sofficeToText(kAsyncEditDocxPath, kStage5TextDir);
        t.check(editSave.success && editDoc.isDirty() && editDoc.currentPath() == kAsyncEditDocxPath
                    && wordsOf(editSavedText) == wordsOf(QStringLiteral("Snapshot text")),
                "edit during a .docx save: file has the snapshot (\"" + editSavedText.trimmed()
                    + "\"), document stays dirty, currentPath is the .docx" + errorNote(editSave));
        const IoRun cleanSave = saveAndWait(&editDoc, kAsyncEditDocxPath);
        t.check(cleanSave.success && !editDoc.isDirty(), "saving again without edits leaves it clean" + errorNote(cleanSave));
    }

    DocxBridge::removeConvertedOdt(kStage4RoundTripPath);
    t.check(QFileInfo::exists(kStage4RoundTripPath), "removeConvertedOdt() refuses a path outside its temp directories");
    const QStringList tempDirsAfter = bridgeTempDirs();
    t.check(tempDirsAfter == tempDirsBefore,
            "no DocxBridge temp directories left behind: " + tempDirsAfter.join(QLatin1String(", ")));

    // Undo / redo. Every controller operation must be exactly one undo
    // step: one undo() restores the document as it was, one redo()
    // reapplies it.
    {
        auto *undoButton = window->findChild<QQuickItem *>(QStringLiteral("undoButton"));
        auto *redoButton = window->findChild<QQuickItem *>(QStringLiteral("redoButton"));
        const auto available = [](QQuickItem *button) { return button && button->property("available").toBool(); };
        const auto select = [&](int start, int end) {
            QMetaObject::invokeMethod(editor, "select", Q_ARG(int, start), Q_ARG(int, end));
        };
        const auto oneStep = [&](const QString &what, const std::function<void()> &op) {
            const QString before = doc->toHtml();
            op();
            const QString after = doc->toHtml();
            controller->undo();
            const bool restored = doc->toHtml() == before;
            controller->redo();
            const bool reapplied = doc->toHtml() == after;
            t.check(after != before && restored && reapplied,
                    QStringLiteral("one undo step: %1 (changed %2, undo restores %3, redo reapplies %4)")
                        .arg(what).arg(after != before).arg(restored).arg(reapplied));
        };

        controller->newDocument();
        editor->forceActiveFocus();
        t.check(!controller->canUndo() && !controller->canRedo() && !available(undoButton) && !available(redoButton),
                "new document: nothing to undo or redo, both buttons unavailable");

        typeText(window, QStringLiteral("One"));
        pressReturn(window);
        typeText(window, QStringLiteral("Two"));
        pressReturn(window);
        typeText(window, QStringLiteral("Three"));
        t.check(controller->canUndo() && available(undoButton) && !available(redoButton),
                "after typing: Undo available, Redo not");

        // Undo/redo move the cursor (collapsing any selection), so each
        // operation selects what it acts on.
        oneStep(QStringLiteral("bold on a selection"), [&] { select(0, 3); controller->toggleBold(); });
        controller->undo();
        t.check(formatOf(doc, 0, 3).isEmpty() && !boldButton->property("active").toBool()
                    && controller->canRedo() && available(redoButton),
                "undoing bold: text plain again, toolbar Bold off, Redo available");
        controller->redo();
        t.check(formatOf(doc, 0, 3) == QStringLiteral("B"), "redo reapplies bold");

        const auto selectAll = [&] { select(0, doc->characterCount() - 1); };
        oneStep(QStringLiteral("alignment over three paragraphs"), [&] { selectAll(); controller->setAlignment(Qt::AlignHCenter); });
        oneStep(QStringLiteral("text color"), [&] { selectAll(); controller->setTextColor(QStringLiteral("#cc0000")); });
        oneStep(QStringLiteral("text color back to automatic"), [&] { selectAll(); controller->setTextColor(QStringLiteral("auto")); });
        oneStep(QStringLiteral("bullet list over three paragraphs"), [&] { selectAll(); controller->toggleBulletList(); });
        oneStep(QStringLiteral("nesting a list item"), [&] { select(4, 7); controller->demoteListItem(); });
        oneStep(QStringLiteral("un-nesting it"), [&] { select(4, 7); controller->promoteListItem(); });
        oneStep(QStringLiteral("removing the list"), [&] { selectAll(); controller->toggleBulletList(); });

        const auto inLastCell = [&] {
            QTextTable *table = nullptr;
            for (auto it = doc->rootFrame()->begin(); !it.atEnd() && !table; ++it)
                table = qobject_cast<QTextTable *>(it.currentFrame());
            if (!table)
                return;
            const int last = table->cellAt(table->rows() - 1, table->columns() - 1).firstCursorPosition().position();
            select(last, last);
        };
        const int end = doc->characterCount() - 1;
        oneStep(QStringLiteral("inserting a table"), [&] { select(end, end); controller->insertTable(2, 2); });
        oneStep(QStringLiteral("inserting a table row"), [&] { inLastCell(); controller->insertTableRow(); });
        oneStep(QStringLiteral("deleting a table column"), [&] { inLastCell(); controller->deleteTableColumn(); });
        oneStep(QStringLiteral("Tab off the last cell (appends a row)"), [&] { inLastCell(); controller->nextTableCell(); });

        // A pending format is applied to the typed text in a second edit;
        // undoing the typing must take it in the same step.
        controller->newDocument();
        typeText(window, QStringLiteral("Plain "));
        controller->toggleBold();
        const QString beforePending = doc->toHtml();
        typeText(window, QStringLiteral("b"));
        const bool pendingApplied = formatOf(doc, 6, 7) == QStringLiteral("B");
        controller->undo();
        t.check(pendingApplied && doc->toHtml() == beforePending,
                QStringLiteral("one undo removes a character typed with a pending bold, format and all (applied %1, text now \"%2\")")
                    .arg(pendingApplied).arg(doc->toPlainText()));
        controller->redo();
        t.check(doc->toPlainText() == QStringLiteral("Plain b") && formatOf(doc, 6, 7) == QStringLiteral("B"),
                "... and one redo brings it back bold");
        controller->newDocument();
        typeText(window, QStringLiteral("Plain "));
        controller->toggleBold();
        const QString beforeBc = doc->toHtml();
        typeText(window, QStringLiteral("bc"));
        const bool continued = doc->toPlainText() == QStringLiteral("Plain bc") && formatOf(doc, 6, 8) == QStringLiteral("B");
        controller->undo();
        t.check(continued && doc->toHtml() == beforeBc,
                "typing on in bold extends the same step: one undo removes \"bc\"");

        // Input-method commits aren't key presses: the pending format is
        // still applied, after insertion (a second undo step there). Qt
        // reports such a commit as its whole paragraph replaced.
        controller->newDocument();
        typeText(window, QStringLiteral("Plain "));
        controller->toggleBold();
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("q"));
        QCoreApplication::sendEvent(editor, &commit);
        t.check(doc->toPlainText() == QStringLiteral("Plain q") && formatOf(doc, 6, 7) == QStringLiteral("B"),
                "an input-method commit with a pending bold is bold: \"" + doc->toPlainText() + "\" " + formatOf(doc, 6, 7));

        // Typing undoes a word at a time (a word plus the spaces after it),
        // not a whole paragraph: Qt alone merges all of it into one step.
        {
            const auto undoAll = [&](int max) {
                QStringList states{doc->toPlainText()};
                for (int i = 0; i < max && controller->canUndo(); ++i) {
                    controller->undo();
                    states.append(doc->toPlainText());
                }
                return states;
            };
            controller->newDocument();
            editor->forceActiveFocus();
            typeText(window, QStringLiteral("The quick  brown fox."));
            const QStringList steps = undoAll(10);
            const QStringList expected{QStringLiteral("The quick  brown fox."), QStringLiteral("The quick  brown "),
                                       QStringLiteral("The quick  "), QStringLiteral("The "), QString()};
            t.check(steps == expected, "typing undoes word by word: " + steps.join(QStringLiteral(" | ")));
            while (controller->canRedo())
                controller->redo();
            t.check(doc->toPlainText() == QStringLiteral("The quick  brown fox."), "... and redoes back to the full text");

            // A second paragraph: Return is its own step, then words again.
            pressReturn(window);
            typeText(window, QStringLiteral("Next line"));
            controller->undo();
            const bool lineWord = doc->toPlainText() == QStringLiteral("The quick  brown fox.\nNext ");
            controller->undo();
            controller->undo();
            t.check(lineWord && doc->toPlainText() == QStringLiteral("The quick  brown fox."),
                    "after Return: words undo one at a time, then the paragraph break: \"" + doc->toPlainText() + "\"");

            // Moving the cursor starts a new step, even mid-word.
            controller->newDocument();
            typeText(window, QStringLiteral("abcdef"));
            select(3, 3);
            typeText(window, QStringLiteral("XY"));
            controller->undo();
            t.check(doc->toPlainText() == QStringLiteral("abcdef"),
                    "text typed after moving the cursor is its own step: \"" + doc->toPlainText() + "\"");
            controller->undo();
            t.check(doc->toPlainText().isEmpty(), "... and the earlier word is another");

            // Typing over a selection: replacing and the new word are one step.
            controller->newDocument();
            typeText(window, QStringLiteral("a quick fox"));
            select(2, 7);
            typeText(window, QStringLiteral("slow"));
            const bool replaced = doc->toPlainText() == QStringLiteral("a slow fox");
            controller->undo();
            t.check(replaced && doc->toPlainText() == QStringLiteral("a quick fox"),
                    "one undo after typing over a selection restores the selected text: \"" + doc->toPlainText() + "\"");

            // Backspace (the TextEdit's own edit) splits typing too.
            controller->newDocument();
            typeText(window, QStringLiteral("wordx"));
            pressKey(window, Qt::Key_Backspace, Qt::NoModifier, QString());
            typeText(window, QStringLiteral("s"));
            controller->undo();
            const bool sGone = doc->toPlainText() == QStringLiteral("word");
            controller->undo();
            t.check(sGone && doc->toPlainText() == QStringLiteral("wordx"),
                    "a Backspace between keystrokes splits the step: \"" + doc->toPlainText() + "\"");

            // Holding a key down: one long word is still one step.
            controller->newDocument();
            typeText(window, QString(200, QLatin1Char('z')));
            controller->undo();
            t.check(doc->isEmpty(), "200 repeated keystrokes in one word: one undo removes them");
        }

        // Undo back to the saved state is clean again, title included.
        // Mid-word ("Plain bc|"), so the save must end the typing step.
        controller->newDocument();
        typeText(window, QStringLiteral("Plain bc"));
        const IoRun saveRun = saveAndWait(controller, kUndoPath);
        typeText(window, QStringLiteral("x"));
        const bool dirtyAfterEdit = controller->isDirty();
        controller->undo();
        t.check(saveRun.success && dirtyAfterEdit && !controller->isDirty()
                    && !window->property("title").toString().contains(QChar(0x25CF)),
                "undoing back to the last save leaves the document clean (title too)" + errorNote(saveRun));
        controller->redo();
        t.check(controller->isDirty(), "redoing past the save makes it dirty again");

        // History doesn't survive New or Open: undo mustn't unbuild a load.
        controller->newDocument();
        t.check(!controller->canUndo() && !controller->canRedo() && !available(undoButton) && !available(redoButton),
                QStringLiteral("New clears the history and both buttons (canUndo %1, canRedo %2, buttons %3/%4)")
                    .arg(controller->canUndo()).arg(controller->canRedo())
                    .arg(available(undoButton)).arg(available(redoButton)));
        typeText(window, QStringLiteral("y"));
        const IoRun reopen = openAndWait(controller, kUndoPath);
        t.check(reopen.success && !controller->canUndo() && !controller->canRedo() && !available(undoButton),
                "opening a file clears the history" + errorNote(reopen));

        // Real Ctrl+Z / Ctrl+Shift+Z through the platform input path: one
        // step per press, whether the editor or something else has focus.
        controller->newDocument();
        editor->forceActiveFocus();
        typeText(window, QStringLiteral("abc"));
        select(0, 3);
        controller->toggleItalic();
        pressSystemKey(window, Qt::Key_Z, Qt::ControlModifier);
        const bool keyUndo = doc->toPlainText() == QStringLiteral("abc") && formatOf(doc, 0, 3).isEmpty();
        pressSystemKey(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        t.check(keyUndo && formatOf(doc, 0, 3) == QStringLiteral("I"),
                "Ctrl+Z / Ctrl+Shift+Z in the editor undo and redo exactly one step");
        window->contentItem()->forceActiveFocus();
        pressSystemKey(window, Qt::Key_Z, Qt::ControlModifier);
        const bool unfocusedUndo = formatOf(doc, 0, 3).isEmpty() && doc->toPlainText() == QStringLiteral("abc");
        pressSystemKey(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        t.check(unfocusedUndo && formatOf(doc, 0, 3) == QStringLiteral("I"),
                "... and with focus outside the editor (window shortcuts)");
        editor->forceActiveFocus();

        // Busy: a .docx open is about to replace the document; undo waits.
        const QString beforeBusy = doc->toHtml();
        bool undoneWhileBusy = true;
        const IoRun busyOpen = runIo(controller, &DocumentController::fileOpened, [&] {
            controller->openFile(kSampleDocx);
            controller->undo();
            undoneWhileBusy = doc->toHtml() != beforeBusy;
        });
        t.check(busyOpen.success && busyOpen.busyAfterCall && !undoneWhileBusy,
                "undo does nothing while a .docx open converts" + errorNote(busyOpen));
    }

    // File flow in Main.qml: Open/Save As dialogs, Save to the current
    // path, the unsaved-changes guard on New/Open/close. The dialogs
    // themselves can't be driven headlessly, so the checks call what their
    // accepted/button handlers call (acceptSave, openPath, resolveDiscard).
    {
        const QStringList dialogs{QStringLiteral("openDialog"), QStringLiteral("saveDialog"),
                                  QStringLiteral("discardDialog")};
        t.check(std::all_of(dialogs.begin(), dialogs.end(),
                            [&](const QString &n) { return window->findChild<QObject *>(n) != nullptr; }),
                "found openDialog, saveDialog and discardDialog");
        const auto anyDialog = [&] {
            return std::any_of(dialogs.begin(), dialogs.end(), [&](const QString &n) { return dialogVisible(window, n); });
        };
        const auto waitSaved = [&](const std::function<void()> &start) {
            return runIo(controller, &DocumentController::fileSaved, start);
        };
        // What QML sees: the title binding only re-reads `dirty` on dirtyChanged().
        const auto titleDirty = [&] { return window->property("title").toString().contains(QChar(0x25CF)); };

        // QTextDocument::clear() resets the modified flag without emitting
        // modificationChanged(); New must still clear the title's marker.
        controller->newDocument();
        typeText(window, QStringLiteral("x"));
        const bool markedAfterTyping = titleDirty();
        controller->newDocument();
        t.check(markedAfterTyping && !titleDirty(),
                "title marks unsaved changes, and New clears the mark: \"" + window->property("title").toString() + "\"");

        controller->newDocument();
        doc->setPlainText(QStringLiteral("Dialog text"));
        doc->setModified(true);
        callRoot(window, "save");
        t.check(dialogVisible(window, QStringLiteral("saveDialog")) && !controller->isBusy(),
                "save() on an untitled document opens Save As instead of writing anywhere");
        closeDialog(window, QStringLiteral("saveDialog"));

        QFile::remove(kDialogOdtPath);
        const IoRun asRun = waitSaved([&] { callRoot(window, "acceptSave", kDialogBasePath); });
        t.check(asRun.success && QFileInfo::exists(kDialogOdtPath) && controller->currentPath() == kDialogOdtPath
                    && !controller->isDirty(),
                "Save As with no extension writes .odt (the first filter) and becomes the current path"
                    + errorNote(asRun));

        QTextCursor(doc).insertText(QStringLiteral("More "));
        const QDateTime before = QFileInfo(kDialogOdtPath).lastModified();
        const IoRun quick = waitSaved([&] { callRoot(window, "save"); });
        t.check(quick.success && !anyDialog() && quick.path == kDialogOdtPath && !controller->isDirty()
                    && QFileInfo(kDialogOdtPath).lastModified() >= before,
                "save() with a current .odt writes it directly, no dialog" + errorNote(quick));

        QFile::remove(kDialogDocxPath);
        const IoRun docxRun = waitSaved([&] { callRoot(window, "acceptSave", kDialogDocxPath); });
        t.check(docxRun.success && controller->currentPath() == kDialogDocxPath && !controller->isDirty(),
                "Save As .docx goes through the bridge and becomes the current path" + errorNote(docxRun));

        // The unsaved-changes guard.
        QTextCursor(doc).insertText(QStringLiteral("Unsaved "));
        callRoot(window, "requestOpen");
        t.check(dialogVisible(window, QStringLiteral("discardDialog")) && !dialogVisible(window, QStringLiteral("openDialog")),
                "Open with unsaved changes asks first");
        closeDialog(window, QStringLiteral("discardDialog"));
        callRoot(window, "resolveDiscard", QStringLiteral("cancel"));
        t.check(!anyDialog() && controller->isDirty() && doc->toPlainText().startsWith(QStringLiteral("Unsaved")),
                "Cancel keeps the document and opens nothing");

        callRoot(window, "requestNew");
        closeDialog(window, QStringLiteral("discardDialog"));
        callRoot(window, "resolveDiscard", QStringLiteral("discard"));
        t.check(doc->isEmpty() && !controller->isDirty() && !titleDirty() && controller->currentPath().isEmpty(),
                "New → Discard: empty, clean (title too), untitled");

        doc->setPlainText(QStringLiteral("Save then new"));
        doc->setModified(true);
        callRoot(window, "requestNew");
        closeDialog(window, QStringLiteral("discardDialog"));
        callRoot(window, "resolveDiscard", QStringLiteral("save"));
        t.check(dialogVisible(window, QStringLiteral("saveDialog")) && !doc->isEmpty(),
                "New → Save on an untitled document goes through Save As first");
        QFile::remove(kDialogOdtPath);
        const IoRun thenNew = waitSaved([&] { callRoot(window, "acceptSave", kDialogOdtPath); });
        closeDialog(window, QStringLiteral("saveDialog"));
        t.check(thenNew.success && doc->isEmpty() && controller->currentPath().isEmpty(),
                "... and the new document follows the successful save" + errorNote(thenNew));

        doc->setPlainText(QStringLiteral("Dropped"));
        doc->setModified(true);
        callRoot(window, "requestNew");
        callRoot(window, "resolveDiscard", QStringLiteral("save"));
        closeDialog(window, QStringLiteral("saveDialog"));
        QMetaObject::invokeMethod(window->findChild<QObject *>(QStringLiteral("saveDialog")), "reject");
        t.check(window->property("pendingAction").isNull() && doc->toPlainText() == QStringLiteral("Dropped"),
                "cancelling that Save As drops the pending New");

        // Open: .odt directly, .doc via the bridge, then Save must not try
        // to write a .doc.
        controller->newDocument();
        const IoRun openOdt = runIo(controller, &DocumentController::fileOpened,
                                    [&] { callRoot(window, "openPath", kDialogOdtPath); });
        t.check(openOdt.success && doc->toPlainText() == QStringLiteral("Save then new")
                    && controller->currentPath() == kDialogOdtPath && !titleDirty(),
                "openPath() opens the .odt Save As wrote" + errorNote(openOdt));
        bool readOnlyWhileBusy = false;
        const IoRun openDoc = runIo(controller, &DocumentController::fileOpened, [&] {
            callRoot(window, "openPath", kSampleDoc);
            readOnlyWhileBusy = editor->property("readOnly").toBool();
        });
        t.check(openDoc.success && openDoc.busyAfterCall && !titleDirty()
                    && controller->currentPath() == QFileInfo(kSampleDoc).absoluteFilePath(),
                "openPath() opens a .doc through the bridge" + errorNote(openDoc));
        t.check(readOnlyWhileBusy && !editor->property("readOnly").toBool(),
                "editor is read-only while the conversion runs, editable once it finishes");
        callRoot(window, "save");
        t.check(dialogVisible(window, QStringLiteral("saveDialog")) && !controller->isBusy(),
                "save() on a .doc opens Save As (.doc can't be written)");
        const QString suggested = window->findChild<QObject *>(QStringLiteral("saveDialog"))
                                      ->property("selectedFile").toUrl().toLocalFile();
        t.check(suggested == QFileInfo(kSampleDoc).absolutePath() + QStringLiteral("/test.odt"),
                "... suggesting the same name as .odt beside it: " + suggested);
        closeDialog(window, QStringLiteral("saveDialog"));

        // Close with unsaved changes is refused until the user decides.
        // Keep the app alive if the guard fails, so the run still reports.
        const bool quitOnClose = QGuiApplication::quitOnLastWindowClosed();
        QGuiApplication::setQuitOnLastWindowClosed(false);
        QTextCursor(doc).insertText(QStringLiteral("x"));
        window->close();
        QCoreApplication::processEvents();
        t.check(window->isVisible() && dialogVisible(window, QStringLiteral("discardDialog")),
                "closing with unsaved changes asks first and keeps the window open");
        closeDialog(window, QStringLiteral("discardDialog"));
        callRoot(window, "resolveDiscard", QStringLiteral("cancel"));
        t.check(window->isVisible() && controller->isDirty(), "Cancel keeps the window and the edits");
        QGuiApplication::setQuitOnLastWindowClosed(quitOnClose);
        controller->newDocument();
    }

    QTextStream(stdout) << (t.ok() ? "PASS" : "FAIL") << Qt::endl;
    return t.ok();
}
