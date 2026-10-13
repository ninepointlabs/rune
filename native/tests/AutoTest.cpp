#include "AutoTest.h"

#include "AiAutoTest.h"
#include "TestSupport.h"

using namespace TestSupport;

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
    // Off until the autosave checks: a save firing during another check's
    // wait would change the state that check is about.
    window->setProperty("autosaveEnabled", false);

    auto *qmlDoc = editor->property("textDocument").value<QQuickTextDocument *>();
    QTextDocument *doc = controller->textDocument();
    t.check(qmlDoc && qmlDoc->textDocument() == doc, "TextEdit renders the controller-owned QTextDocument");

    controller->newDocument();
    t.check(doc->isEmpty() && !controller->isDirty(), "newDocument(): empty and clean");

    Env env{t, window, controller, editor, doc};
    testFormatting(env);
    if (!testTables(env))
        return false;
    const QString roundTripText = testOdf(env);
    // Neither .docx conversions nor the asynchronous open/save leave
    // DocxBridge temporary directories behind.
    const QStringList tempDirsBefore = bridgeTempDirs();
    testDocx(env, roundTripText);
    testAsyncIo(env, roundTripText);
    const QStringList tempDirsAfter = bridgeTempDirs();
    t.check(tempDirsAfter == tempDirsBefore,
            "no DocxBridge temp directories left behind: " + tempDirsAfter.join(QLatin1String(", ")));
    testUndo(env);
    testPages(env);
    testHeaderFooter(env);

    // The AI layer (AiAutoTest.cpp): mock servers, no real network.
    runAiAutoTest(window, [&t](bool ok, const QString &what) { t.check(ok, what); });

    testWindow(env);

    QTextStream(stdout) << (t.ok() ? "PASS" : "FAIL") << Qt::endl;
    return t.ok();
}
