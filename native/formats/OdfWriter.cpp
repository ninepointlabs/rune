#include "OdfWriter.h"

#include "Package.h"

#include <QBuffer>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

QString dataEntryName(const QString &store)
{
    return QStringLiteral("Rune/") + store + QStringLiteral(".json");
}

QByteArray write(const QTextDocument *body, const QByteArray &stylesXml, const DocumentData::Stores &data)
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

    std::optional<QList<Package::Entry>> entries = Package::read(buffer.data());
    const Package::Entry *found = entries ? Package::find(*entries, QStringLiteral("META-INF/manifest.xml")) : nullptr;
    if (!found || !found->data.contains("</manifest:manifest>")) {
        qWarning() << "OdfWriter: cannot read back Qt's ODF archive";
        return {};
    }
    QByteArray manifest = found->data;
    // Added at the end, each listed in the manifest. Qt's writer leaves out
    // styles.xml (the page layout, header and footer).
    QByteArray manifestEntries;
    const auto add = [&](const QString &name, const QByteArray &bytes, const char *mediaType) {
        if (Package::Entry *existing = Package::find(*entries, name))
            existing->data = bytes;
        else
            entries->append({name, bytes});
        const QByteArray listing = "manifest:full-path=\"" + name.toUtf8() + '"';
        if (!manifest.contains(listing))
            manifestEntries += "<manifest:file-entry manifest:media-type=\"" + QByteArray(mediaType) + "\" " + listing + "/>";
    };
    add(QStringLiteral("styles.xml"), stylesXml, "text/xml");
    for (auto it = data.cbegin(); it != data.cend(); ++it) {
        const QJsonDocument json = it.value().isArray() ? QJsonDocument(it.value().toArray())
                                                        : QJsonDocument(it.value().toObject());
        add(dataEntryName(it.key()), json.toJson(QJsonDocument::Indented), "application/json");
    }
    manifest.insert(manifest.lastIndexOf("</manifest:manifest>"), manifestEntries);
    Package::find(*entries, QStringLiteral("META-INF/manifest.xml"))->data = manifest;

    const QByteArray odt = Package::write(*entries);
    if (odt.isEmpty())
        qWarning() << "OdfWriter: cannot write the ODF archive";
    return odt;
}

} // namespace OdfWriter
