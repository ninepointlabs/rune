// .docx and .doc via LibreOffice batch conversion (DocxBridge).

#include "TestSupport.h"

namespace TestSupport {

void testDocx(Env &env, const QString &roundTripText)
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

    // --- Stage 5: .docx via LibreOffice batch conversion ---


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
    selectWord(editor, doc, QStringLiteral("Quarterly Report"));
    controller->toggleBold();
    selectWord(editor, doc, QStringLiteral("bold"));
    controller->toggleBold();
    selectWord(editor, doc, QStringLiteral("italic"));
    controller->toggleItalic();
    const QString docxWritten = doc->toPlainText();
    // What actually goes into the file: the sanitizer adds an empty
    // paragraph between the list and the table.
    std::unique_ptr<QTextDocument> exported(doc->clone());
    OdfWriter::sanitize(exported.get());
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
            "openFile(corrupt .docx) -> fileOpened(false), not busy, document unchanged");
    QString bridgeError;
    const bool corruptFailed = DocxBridge::convertToOdt(kStage5CorruptPath, &bridgeError).isEmpty();
    // LibreOffice versions differ on a corrupt file: some exit non-zero
    // (26.x), others exit 0 having written nothing (25.x, e.g. Debian 13,
    // Fedora 42). Either way the bridge must fail and say which.
    const bool exitedNonZero = bridgeError.startsWith(QStringLiteral("LibreOffice failed converting"))
        && bridgeError.contains(QStringLiteral("(exit "));
    const bool wroteNothing = bridgeError.startsWith(QStringLiteral("LibreOffice reported success but wrote no .odt"));
    t.check(corruptFailed && (exitedNonZero || wroteNothing),
            "DocxBridge::convertToOdt(corrupt) fails, saying LibreOffice exited non-zero or wrote nothing: \""
                + bridgeError + "\"");
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
}

} // namespace TestSupport
