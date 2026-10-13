#pragma once

// What every --auto-test file shares: the checker, the files the checks
// write (all under /tmp), and helpers for driving the window and reading
// documents back. Header-only (inline), for the test files in tests/.

#include "DocumentController.h"
#include "DocxBridge.h"
#include "OdfReader.h"
#include "OdfWriter.h"
#include "PageLayout.h"
#include "PageParts.h"

#include <QCoreApplication>
#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QFontMetricsF>
#include <QTextLayout>
#include <QStyleHints>
#include <QPainter>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QProcess>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocumentWriter>
#include <QTextFrame>
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

namespace TestSupport {

inline const QString kStage1Path = QStringLiteral("/tmp/rune-native-stage1.odt");
inline const QString kSanitizePath = QStringLiteral("/tmp/rune-native-sanitize.odt");
inline const QString kStage2AlignPath = QStringLiteral("/tmp/rune-native-stage2-align.odt");
inline const QString kStage2ListPath = QStringLiteral("/tmp/rune-native-stage2-list.odt");
inline const QString kStage2FontPath = QStringLiteral("/tmp/rune-native-stage2-font.odt");
inline const QString kStage2ColorPath = QStringLiteral("/tmp/rune-native-stage2-color.odt");
inline const QString kStage3ListTablePath = QStringLiteral("/tmp/rune-native-stage3-list-table.odt");
inline const QString kStage3ListTableRawPath = QStringLiteral("/tmp/rune-native-stage3-list-table-unsanitized.odt");
inline const QString kStage3TablePath = QStringLiteral("/tmp/rune-native-stage3-table.odt");
inline const QString kStage3TabPath = QStringLiteral("/tmp/rune-native-stage3-tab.odt");
inline const QString kStage4RoundTripPath = QStringLiteral("/tmp/rune-native-stage4-roundtrip.odt");
inline const QString kStage4ExternalDir = QStringLiteral("/tmp/rune-native-stage4-external");
inline const QString kStage4MissingPath = QStringLiteral("/tmp/rune-native-stage4-does-not-exist.odt");
inline const QString kStage4bListPath = QStringLiteral("/tmp/rune-native-stage4b-list.odt");
inline const QString kStage4bTablePath = QStringLiteral("/tmp/rune-native-stage4b-table.odt");
inline const QString kStage4bFontPath = QStringLiteral("/tmp/rune-native-stage4b-font.odt");
inline const QString kStage4bInheritPath = QStringLiteral("/tmp/rune-native-stage4b-inherit.odt");
inline const QString kStage4NotZipPath = QStringLiteral("/tmp/rune-native-stage4-not-a-zip.odt");
inline const QString kStage4NoContentPath = QStringLiteral("/tmp/rune-native-stage4-no-content.odt");
inline const QString kStage4BadXmlPath = QStringLiteral("/tmp/rune-native-stage4-bad-xml.odt");
inline const QString kStage5DocxPath = QStringLiteral("/tmp/rune-native-stage5-roundtrip.docx");
inline const QString kStage5TextDir = QStringLiteral("/tmp/rune-native-stage5-txt");
inline const QString kStage5MissingPath = QStringLiteral("/tmp/rune-native-stage5-does-not-exist.docx");
inline const QString kStage5TxtPath = QStringLiteral("/tmp/rune-native-stage5-plain.txt");
inline const QString kStage5NoExtPath = QStringLiteral("/tmp/rune-native-stage5-no-extension");
inline const QString kStage5CorruptPath = QStringLiteral("/tmp/rune-native-stage5-corrupt.docx");
inline const QString kStage5UnwritablePath = QStringLiteral("/tmp/rune-native-stage5-no-such-dir/out.docx");
inline const QString kAsyncOdtPath = QStringLiteral("/tmp/rune-native-async.odt");
inline const QString kAsyncEditDocxPath = QStringLiteral("/tmp/rune-native-async-edited.docx");
inline const QString kUndoPath = QStringLiteral("/tmp/rune-native-undo.odt");
inline const QString kDialogBasePath = QStringLiteral("/tmp/rune-native-dialog");
inline const QString kDialogOdtPath = kDialogBasePath + QStringLiteral(".odt");
inline const QString kDialogDocxPath = QStringLiteral("/tmp/rune-native-dialog.docx");
inline const QString kPagePath = QStringLiteral("/tmp/rune-native-pages.odt");
inline const QString kPageDocxPath = QStringLiteral("/tmp/rune-native-pages.docx");
inline const QString kPageBarePath = QStringLiteral("/tmp/rune-native-pages-bare.odt");
inline const QString kFontSizePath = QStringLiteral("/tmp/rune-native-font-size.odt");
inline const QString kPartsPath = QStringLiteral("/tmp/rune-native-header-footer.odt");
inline const QString kPartsDocxPath = QStringLiteral("/tmp/rune-native-header-footer.docx");
inline const QString kAutosaveDir = QStringLiteral("/tmp/rune-native-autosave");
inline const QString kAutosaveOdtPath = kAutosaveDir + QStringLiteral("/autosave.odt");
inline const QString kAutosaveDocxPath = kAutosaveDir + QStringLiteral("/autosave.docx");
inline const QString kSampleDocx = QStringLiteral(RUNE_SAMPLES_DIR "/test.docx");
inline const QString kSampleDoc = QStringLiteral(RUNE_SAMPLES_DIR "/test.doc");

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
inline void typeText(QQuickWindow *window, const QString &text)
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
inline QString formatOf(QTextDocument *doc, int start, int end)
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
inline QTextBlock blockBeforeFirstTable(QTextDocument *doc)
{
    QTextBlock previous;
    for (auto it = doc->rootFrame()->begin(); !it.atEnd(); ++it) {
        if (qobject_cast<QTextTable *>(it.currentFrame()))
            return previous;
        previous = it.currentFrame() ? QTextBlock() : it.currentBlock();
    }
    return {};
}

inline int listIndent(const QTextBlock &block)
{
    return block.textList() ? block.textList()->format().indent() : 0;
}

// A real key press + release through the window.
inline void pressKey(QQuickWindow *window, int key, Qt::KeyboardModifiers modifiers, const QString &text)
{
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QCoreApplication::sendEvent(window, &press);
    QCoreApplication::sendEvent(window, &release);
}

// A key press + release through the platform input path, as a real
// keyboard delivers it: unlike sendEvent(), this goes through Qt's shortcut
// handling (ShortcutOverride, then Shortcut items) before the focus item.
inline void pressSystemKey(QQuickWindow *window, Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    QTest::keyClick(window, key, modifiers);
    QCoreApplication::processEvents();
}

// Real Return key, which TextEdit turns into a new paragraph block.
inline void pressReturn(QQuickWindow *window)
{
    pressKey(window, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
}

inline void pressTab(QQuickWindow *window)
{
    pressKey(window, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
}

inline void pressBacktab(QQuickWindow *window)
{
    pressKey(window, Qt::Key_Backtab, Qt::ShiftModifier, QString());
}

// The item named `name` under `root` in the visual tree. Unlike findChild(),
// this finds Repeater-made items, whose QObject parent isn't the window's.
inline QQuickItem *findItem(QQuickItem *root, const QString &name)
{
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem *child : root->childItems())
        if (QQuickItem *found = findItem(child, name))
            return found;
    return nullptr;
}

// The table the TextEdit's cursor is in, if any.
inline QTextTable *tableAtCursor(QTextDocument *doc, QQuickItem *editor)
{
    QTextCursor cursor(doc);
    cursor.setPosition(editor->property("cursorPosition").toInt());
    return cursor.currentTable();
}

// The TextEdit's cursor cell as "row,column", or "none".
inline QString cellAtCursor(QTextDocument *doc, QQuickItem *editor)
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
inline QString convertWithSoffice(const QString &odtPath)
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

inline int countOf(const QString &html, const QString &tag)
{
    return int(html.count(QRegularExpression(QStringLiteral("<%1[\\s>]").arg(tag),
                                             QRegularExpression::CaseInsensitiveOption)));
}

// The HTML's text content, tags stripped and whitespace collapsed.
inline QString textOf(const QString &html)
{
    QString body = html.mid(html.indexOf(QLatin1String("<body"), 0, Qt::CaseInsensitive));
    body.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    return body.simplified();
}

// Reports what soffice made of a saved file, for the log.
inline void logConversion(const QString &odtPath, const QString &html)
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
inline QString sofficeHtmlToOdt(const QString &htmlPath, const QString &outDir)
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
inline bool writeZip(const QString &path, const QString &name, const QByteArray &data)
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
inline QString blocksOf(QTextDocument *doc)
{
    QStringList blocks;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next())
        blocks << '"' + block.text().replace('\t', QLatin1String("\\t")) + '"';
    return blocks.join(QLatin1String(", "));
}

// Where `word` starts in the plain text, as a document position.
inline int positionOf(QTextDocument *doc, const QString &word)
{
    return doc->toPlainText().indexOf(word);
}

inline QTextBlock blockWithText(QTextDocument *doc, const QString &text)
{
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next())
        if (block.text() == text)
            return block;
    return {};
}

// A zip with several entries, for hand-built ODF test fixtures.
inline bool writeZipMulti(const QString &path, const QList<QPair<QString, QByteArray>> &entries)
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
inline QString sofficeToText(const QString &path, const QString &outDir)
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
inline QString wordsOf(QString text)
{
    text.remove(QChar(0xFEFF)).remove(QChar(0x2022));
    return text.simplified();
}

inline bool listKindIsBullet(const QTextList *list)
{
    const QTextListFormat::Style style = list->format().style();
    return style == QTextListFormat::ListDisc || style == QTextListFormat::ListCircle
        || style == QTextListFormat::ListSquare;
}

// DocxBridge's temporary directories currently in the temp dir.
inline QStringList bridgeTempDirs()
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
inline bool waitUntil(const std::function<bool()> &done, int timeoutMs = kIoTimeoutMs)
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
inline IoRun runIo(DocumentController *controller, Signal completion, const std::function<void()> &start)
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

inline IoRun openAndWait(DocumentController *controller, const QString &path)
{
    return runIo(controller, &DocumentController::fileOpened, [=] { controller->openFile(path); });
}

inline IoRun saveAndWait(DocumentController *controller, const QString &path)
{
    return runIo(controller, &DocumentController::fileSaved, [=] { controller->saveFile(path); });
}

// " (error: ...)" for a check's label, if there was one.
inline QString errorNote(const IoRun &run)
{
    if (!run.fired)
        return QStringLiteral(" (no completion signal within %1 s)").arg(kIoTimeoutMs / 1000);
    return run.error.isEmpty() ? QString() : QStringLiteral(" (error: %1)").arg(run.error);
}

// The zip entry names in `path` ("" if not a zip).
inline QStringList zipEntries(const QString &path)
{
    QuaZip zip(path);
    return zip.open(QuaZip::mdUnzip) ? zip.getFileNameList() : QStringList();
}

// Calls a Main.qml root function, as the dialogs and shortcuts do.
inline void callRoot(QQuickWindow *window, const char *function, const QVariant &arg = {})
{
    if (arg.isValid())
        QMetaObject::invokeMethod(window, function, Q_ARG(QVariant, arg));
    else
        QMetaObject::invokeMethod(window, function);
}

inline bool dialogVisible(QQuickWindow *window, const QString &name)
{
    QObject *dialog = window->findChild<QObject *>(name);
    return dialog && dialog->property("visible").toBool();
}

inline void closeDialog(QQuickWindow *window, const QString &name)
{
    if (QObject *dialog = window->findChild<QObject *>(name))
        QMetaObject::invokeMethod(dialog, "close");
}

// QPlatformDialogHelper's StandardButton / ButtonRole values (qpa API, not
// exposed to QML except through MessageDialog's enums).
constexpr int kSaveButton = 0x00000800, kDiscardButton = 0x00800000, kCancelButton = 0x00400000;
constexpr int kAcceptRole = 0, kRejectRole = 1, kDestructiveRole = 2;

// What a MessageDialog emits when the user clicks one of its buttons, so
// the QML handler runs as for a real click. Hides the dialog first, as a
// click does.
inline bool clickMessageButton(QQuickWindow *window, const QString &name, int button, int role)
{
    QObject *dialog = window->findChild<QObject *>(name);
    if (!dialog)
        return false;
    QMetaObject::invokeMethod(dialog, "close");
    const int index = dialog->metaObject()->indexOfSignal(
        "buttonClicked(QPlatformDialogHelper::StandardButton,QPlatformDialogHelper::ButtonRole)");
    if (index < 0)
        return false;
    const bool invoked = dialog->metaObject()->method(index).invoke(
        dialog, Qt::DirectConnection, QGenericArgument("QPlatformDialogHelper::StandardButton", &button),
        QGenericArgument("QPlatformDialogHelper::ButtonRole", &role));
    QCoreApplication::processEvents(); // the dialog's deferred rejected() handling
    return invoked;
}

// Selects `word` (its first occurrence in `doc`) in the editor, as a
// mouse selection would.
inline void selectWord(QQuickItem *editor, QTextDocument *doc, const QString &word)
{
    const int start = positionOf(doc, word);
    QMetaObject::invokeMethod(editor, "select", Q_ARG(int, start), Q_ARG(int, int(start + word.size())));
}

// formatOf() the first occurrence of `word`, or "missing".
inline QString formatOfWord(QTextDocument *d, const QString &word)
{
    const int start = positionOf(d, word);
    return start < 0 ? QStringLiteral("missing") : formatOf(d, start, int(start + word.size()));
}

} // namespace TestSupport

namespace TestSupport {

// The window and what the checks work through: its controller, the body's
// TextEdit and document.
struct Env
{
    Checker &t;
    QQuickWindow *window;
    DocumentController *controller;
    QQuickItem *editor;
    QTextDocument *doc;
};

// The checks, by feature, in the order runAutoTest() runs them. testTables()
// is false if the window lacks what the rest of the run needs. testOdf()
// leaves its round-trip file open and returns its text, which testDocx()
// and testAsyncIo() check failed opens leave alone.
void testFormatting(Env &env);
bool testTables(Env &env);
QString testOdf(Env &env);
void testDocx(Env &env, const QString &roundTripText);
void testAsyncIo(Env &env, const QString &roundTripText);
void testUndo(Env &env);
void testPages(Env &env);
void testHeaderFooter(Env &env);
void testRegistries(Env &env);
void testDocumentData(Env &env);
void testWindow(Env &env);

} // namespace TestSupport
