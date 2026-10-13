// Reading ODF: round trips through the UI, files LibreOffice wrote, styles
// and inheritance, and failures leaving the open document alone.

#include "TestSupport.h"

namespace TestSupport {

QString testOdf(Env &env)
{
    Checker &t = env.t;
    QQuickWindow *window = env.window;
    DocumentController *controller = env.controller;
    QQuickItem *editor = env.editor;
    QTextDocument *doc = env.doc;
    auto *qmlDoc = editor->property("textDocument").value<QQuickTextDocument *>();

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
    selectWord(editor, doc, QStringLiteral("Title"));
    controller->toggleBold();
    selectWord(editor, doc, QStringLiteral("bold"));
    controller->toggleBold();
    selectWord(editor, doc, QStringLiteral("italic"));
    controller->toggleItalic();
    selectWord(editor, doc, QStringLiteral("under"));
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
    return written;
}

} // namespace TestSupport
