#include "AutoTest.h"

#include "DocumentController.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTextBlock>
#include <QTextList>
#include <QTextTable>
#include <QTextStream>

#include <memory>

namespace {

const QString kStage1Path = QStringLiteral("/tmp/rune-native-stage1.odt");
const QString kSanitizePath = QStringLiteral("/tmp/rune-native-sanitize.odt");

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

} // namespace

bool runAutoTest(QQuickWindow *window)
{
    Checker t;
    QTextStream(stdout) << "rune_native --auto-test" << Qt::endl;

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

    QTextStream(stdout) << (t.ok() ? "PASS" : "FAIL") << Qt::endl;
    return t.ok();
}
