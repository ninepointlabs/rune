#include "OdfExporter.h"

#include <QDebug>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentWriter>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

bool OdfExporter::exportToOdf(QQuickTextDocument *doc, const QString &path)
{
    if (!doc || !doc->textDocument()) {
        qWarning() << "OdfExporter: no text document";
        return false;
    }
    QTextDocumentWriter writer(path, QByteArrayLiteral("ODF"));
    if (!writer.write(doc->textDocument())) {
        qWarning() << "OdfExporter: write failed for" << path
                   << "device error:" << (writer.device() ? writer.device()->errorString() : QString());
        return false;
    }
    return true;
}

namespace {

QString charSummary(const QTextCharFormat &f)
{
    QStringList bits;
    if (f.fontWeight() >= QFont::Bold)
        bits << "bold";
    if (f.fontItalic())
        bits << "italic";
    if (f.fontUnderline())
        bits << "underline";
    return bits.join('+');
}

void describeBlock(const QTextBlock &block, int indent, QStringList &out)
{
    const QString pad(indent * 2, ' ');
    QString line = pad + QStringLiteral("block");
    if (const int level = block.blockFormat().headingLevel())
        line += QStringLiteral(" h%1").arg(level);
    if (QTextList *list = block.textList())
        line += QStringLiteral(" list(style=%1 indent=%2 item=%3)")
                    .arg(list->format().style())
                    .arg(list->format().indent())
                    .arg(list->itemNumber(block));
    line += QStringLiteral(": \"%1\"").arg(block.text());
    out << line;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment frag = it.fragment();
        const QString fmt = charSummary(frag.charFormat());
        if (!fmt.isEmpty())
            out << pad + QStringLiteral("  span[%1]: \"%2\"").arg(fmt, frag.text());
    }
}

void describeFrame(QTextFrame *frame, int indent, QStringList &out)
{
    const QString pad(indent * 2, ' ');
    if (auto *table = qobject_cast<QTextTable *>(frame)) {
        out << pad + QStringLiteral("table %1x%2 (headerRows=%3)")
                         .arg(table->rows())
                         .arg(table->columns())
                         .arg(table->format().headerRowCount());
        for (int r = 0; r < table->rows(); ++r) {
            QStringList cells;
            for (int c = 0; c < table->columns(); ++c) {
                const QTextTableCell cell = table->cellAt(r, c);
                QString text;
                for (auto it = cell.begin(); !it.atEnd(); ++it)
                    if (it.currentBlock().isValid())
                        text += it.currentBlock().text();
                cells << text;
            }
            out << pad + QStringLiteral("  row %1: [%2]").arg(r).arg(cells.join(" | "));
        }
        return;
    }
    for (auto it = frame->begin(); !it.atEnd(); ++it) {
        if (QTextFrame *child = it.currentFrame())
            describeFrame(child, indent + 1, out);
        else if (it.currentBlock().isValid())
            describeBlock(it.currentBlock(), indent + 1, out);
    }
}

} // namespace

QString OdfExporter::describe(QQuickTextDocument *doc) const
{
    if (!doc || !doc->textDocument())
        return QStringLiteral("<no document>");
    QStringList out{QStringLiteral("root frame")};
    describeFrame(doc->textDocument()->rootFrame(), 0, out);
    return out.join('\n');
}
