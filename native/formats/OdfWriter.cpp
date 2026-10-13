#include "OdfWriter.h"

#include "PageLayout.h"

#include <QBuffer>
#include <QDebug>
#include <QList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentWriter>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

#include <algorithm>
#include <memory>

namespace OdfWriter {

namespace {

// Rule 1: list directly followed by a table.
//
// Qt's ODF writer (QTextDocumentWriter, format "ODF") emits malformed XML
// when a table immediately follows a list block with no plain paragraph in
// between, and LibreOffice then silently drops the whole table. Found and
// verified in native-spike/ (XML inspection plus a real soffice round-trip
// showing the data loss); the verified fix is an empty paragraph between
// the list and the table. A table made by QTextCursor::insertTable() at the
// end of a list item (Tables::insert()) has the same shape as one parsed
// by setHtml(): a root-frame child right after the list block, with the
// same malformed output when unsanitized.
//
// Collects the end-of-text position of every list block that is directly
// followed by a table, walking nested frames and table cells.
void collectListTableJoins(QTextFrame::iterator it, QList<int> &positions)
{
    QTextBlock previous;
    for (; !it.atEnd(); ++it) {
        QTextFrame *child = it.currentFrame();
        if (!child) {
            previous = it.currentBlock();
            continue;
        }
        auto *table = qobject_cast<QTextTable *>(child);
        if (table && previous.isValid() && previous.textList())
            positions << previous.position() + previous.length() - 1;
        if (table) {
            for (int r = 0; r < table->rows(); ++r)
                for (int c = 0; c < table->columns(); ++c) {
                    const QTextTableCell cell = table->cellAt(r, c);
                    if (cell.row() == r && cell.column() == c) // spanned cells once
                        collectListTableJoins(cell.begin(), positions);
                }
        } else {
            collectListTableJoins(child->begin(), positions);
        }
        previous = QTextBlock();
    }
}

} // namespace

void sanitize(QTextDocument *doc)
{
    if (!doc)
        return;

    QList<int> joins;
    collectListTableJoins(doc->rootFrame()->begin(), joins);
    // Back to front so earlier positions stay valid.
    std::sort(joins.begin(), joins.end(), std::greater<int>());
    for (const int position : std::as_const(joins)) {
        QTextCursor cursor(doc);
        cursor.setPosition(position);
        // A default block format carries no list membership.
        cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    }
}

QByteArray write(const QTextDocument *body, const QByteArray &stylesXml)
{
    // Sanitize a copy: the export workarounds must not rewrite what the
    // user is editing.
    std::unique_ptr<QTextDocument> copy(body->clone());
    sanitize(copy.get());

    // The ODF writer close()s its device, so render into memory.
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QTextDocumentWriter writer(&buffer, QByteArrayLiteral("ODF"));
    if (!writer.write(copy.get())) {
        qWarning() << "OdfWriter: ODF serialisation failed";
        return {};
    }
    // Qt's writer leaves out the page layout, header and footer (it writes
    // no styles.xml).
    const QByteArray odt = withOdfStyles(buffer.data(), stylesXml);
    if (odt.isEmpty())
        qWarning() << "OdfWriter: cannot add the page layout to the ODF archive";
    return odt;
}

} // namespace OdfWriter
