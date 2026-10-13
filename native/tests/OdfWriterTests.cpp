// Rune's own ODF writer (OdfTextWriter, OdfWriter): what Qt's writer lost or
// got wrong, through Rune reopening the file and through LibreOffice.

#include "TestSupport.h"

#include "Package.h"

namespace TestSupport {

namespace {

const QString kWriterPath = QStringLiteral("/tmp/rune-native-writer.odt");
const QString kWriterTextDir = QStringLiteral("/tmp/rune-native-writer-txt");
const QString kWriterDocxPath = QStringLiteral("/tmp/rune-native-writer.docx");
const QString kNestedTablePath = QStringLiteral("/tmp/rune-native-writer-nested.odt");

} // namespace

void testOdfWriter(Env &env)
{
    Checker &t = env.t;

    DocumentController writer;
    QTextDocument *doc = writer.textDocument();
    // Headings, a link, strikethrough and superscript, a table with a
    // merged cell, nested lists.
    doc->setHtml(QStringLiteral(
        "<h1>Chapter One</h1><h2>The Harbour</h2>"
        "<p>See <a href=\"https://example.com/a?b=1&amp;c=2\">the source</a>, <s>struck</s> and x<sup>2</sup>.</p>"
        "<table border=\"1\" width=\"100%\"><tr><td colspan=\"2\">Wide</td></tr><tr><td>Left</td><td>Right</td></tr></table>"
        "<ul><li>Outer<ul><li>Inner</li></ul></li><li>Outer two</li></ul>"));
    // A numbered list interrupted by a paragraph and resumed: one list.
    QTextCursor c(doc);
    c.movePosition(QTextCursor::End);
    c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    QTextListFormat numbered;
    numbered.setStyle(QTextListFormat::ListDecimal);
    QTextList *list = c.createList(numbered);
    c.insertText(QStringLiteral("First"));
    c.insertBlock();
    c.insertText(QStringLiteral("Second"));
    c.insertBlock();
    list->remove(c.block());
    QTextBlockFormat plain;
    plain.setIndent(0);
    plain.setObjectIndex(-1);
    c.setBlockFormat(plain);
    c.insertText(QStringLiteral("Interruption"));
    c.insertBlock();
    list->add(c.block());
    c.insertText(QStringLiteral("Third"));
    const QString written = doc->toPlainText();

    t.check(writer.saveToOdf(kWriterPath), "saveToOdf() with headings, a link, a merged cell, nested and resumed lists");
    const std::optional<QList<Package::Entry>> entries = Package::readFile(kWriterPath);
    const Package::Entry *contentEntry = entries ? Package::find(*entries, QStringLiteral("content.xml")) : nullptr;
    const QString content = contentEntry ? QString::fromUtf8(contentEntry->data) : QString();
    QStringList names;
    if (entries)
        for (const Package::Entry &entry : *entries)
            names << entry.name;
    t.check(!names.isEmpty() && names.constFirst() == QStringLiteral("mimetype")
                && names.contains(QStringLiteral("meta.xml")) && names.contains(QStringLiteral("META-INF/manifest.xml")),
            "the package: mimetype first, content, styles, meta.xml and a manifest: " + names.join(QStringLiteral(", ")));
    t.check(content.contains(QStringLiteral("<text:h text:style-name=\"P")) && content.contains(QStringLiteral("text:outline-level=\"2\""))
                && content.contains(QStringLiteral("xlink:href=\"https://example.com/a?b=1&amp;c=2\""))
                && content.contains(QStringLiteral("table:number-columns-spanned=\"2\""))
                && content.contains(QStringLiteral("<table:covered-table-cell/>"))
                && content.contains(QStringLiteral("style:rel-width=\"100%\""))
                && content.contains(QStringLiteral("text:continue-numbering=\"true\"")),
            "content.xml: <text:h> with outline levels, the link, the merged cell, a 100% wide table, the resumed list");

    DocumentController reader;
    const bool read = reader.openOdf(kWriterPath);
    QTextDocument *rdoc = reader.textDocument();
    const QTextBlock h1 = blockWithText(rdoc, QStringLiteral("Chapter One"));
    const QTextBlock h2 = blockWithText(rdoc, QStringLiteral("The Harbour"));
    t.check(read && rdoc->toPlainText() == written && h1.blockFormat().headingLevel() == 1
                && h2.blockFormat().headingLevel() == 2,
            "reopened: the same text, and the headings are headings again (level 1 and 2)");
    const QTextBlock inner = blockWithText(rdoc, QStringLiteral("Inner"));
    const QTextBlock third = blockWithText(rdoc, QStringLiteral("Third"));
    t.check(inner.textList() && inner.textList()->format().indent() == 2 && third.textList()
                && listKindIsBullet(blockWithText(rdoc, QStringLiteral("Outer")).textList())
                && !listKindIsBullet(third.textList()),
            "reopened: the nested bullet item one level in, the resumed item still numbered");

    const QString html = convertWithSoffice(kWriterPath);
    logConversion(kWriterPath, html);
    t.check(countOf(html, QStringLiteral("h1")) == 1 && countOf(html, QStringLiteral("h2")) == 1,
            "LibreOffice: the headings are headings (one <h1>, one <h2>)");
    t.check(html.contains(QStringLiteral("href=\"https://example.com/a?b=1&amp;c=2\"")),
            "LibreOffice: the link, its address intact");
    t.check(html.contains(QStringLiteral("colspan=\"2\""), Qt::CaseInsensitive) && countOf(html, QStringLiteral("td")) == 3,
            "LibreOffice: the merged cell spans two columns (3 cells in all)");
    const QString text = sofficeToText(kWriterPath, kWriterTextDir);
    QTextStream(stdout) << "  soffice writer.odt -> txt: \"" << QString(text).replace(u'\n', u'|') << "\"" << Qt::endl;
    t.check(text.contains(QStringLiteral("3. Third")) || text.contains(QStringLiteral("3.\tThird")),
            "LibreOffice: the resumed list numbers on (Third is 3.)");

    // A table in a table cell (lost by Qt's writer).
    {
        DocumentController nested;
        nested.textDocument()->setHtml(QStringLiteral(
            "<table border=\"1\"><tr><td>Outer cell<table border=\"1\"><tr><td>Inner one</td><td>Inner two</td></tr>"
            "</table></td><td>Beside</td></tr></table>"));
        const bool saved = nested.saveToOdf(kNestedTablePath);
        DocumentController nestedReader;
        const bool reread = nestedReader.openOdf(kNestedTablePath);
        const QString nestedHtml = convertWithSoffice(kNestedTablePath);
        logConversion(kNestedTablePath, nestedHtml);
        t.check(saved && reread && nestedReader.textDocument()->toPlainText() == nested.textDocument()->toPlainText()
                    && countOf(nestedHtml, QStringLiteral("table")) == 2
                    && textOf(nestedHtml).contains(QStringLiteral("Inner one")) && textOf(nestedHtml).contains(QStringLiteral("Beside")),
                "a table inside a table cell: saved, reopened the same, and LibreOffice reads both tables and all their text");
    }

    // Word: headings in Word's own heading styles.
    const IoRun docx = saveAndWait(&writer, kWriterDocxPath);
    const std::optional<QList<Package::Entry>> docxEntries = Package::readFile(kWriterDocxPath);
    const Package::Entry *body = docxEntries ? Package::find(*docxEntries, QStringLiteral("word/document.xml")) : nullptr;
    const QByteArray bodyXml = body ? body->data : QByteArray();
    t.check(docx.success && bodyXml.contains("<w:pStyle w:val=\"Heading1\"/>") && bodyXml.contains("<w:pStyle w:val=\"Heading2\"/>"),
            "as .docx: the headings in Word's Heading 1 and Heading 2 styles" + errorNote(docx));
}

} // namespace TestSupport
