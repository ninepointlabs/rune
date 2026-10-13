// Asynchronous openFile()/saveFile(): busy state, re-entrancy, the main
// thread staying free, edits during a .docx save.

#include "TestSupport.h"

namespace TestSupport {

void testAsyncIo(Env &env, const QString &roundTripText)
{
    Checker &t = env.t;
    QQuickWindow *window = env.window;
    DocumentController *controller = env.controller;
    QQuickItem *editor = env.editor;
    QTextDocument *doc = env.doc;
    // testOdf()'s file, still open: what failed opens must leave alone.
    const auto unchanged = [&] {
        return doc->toPlainText() == roundTripText && controller->currentPath() == kStage4RoundTripPath
            && !controller->isDirty();
    };

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
}

} // namespace TestSupport
