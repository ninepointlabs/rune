#include "OdfTextWriter.h"

#include "FormatIds.h"
#include "InlineObjects.h"
#include "Lists.h"

#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

#include <algorithm>

namespace {

constexpr qreal kMmPerPx = 25.4 / 96.0;
constexpr qreal kPtPerPx = 72.0 / 96.0;
// The levels every list style defines.
constexpr int kListLevels = 10;

QString mm(qreal value)
{
    return QString::number(value, 'f', 2) + QStringLiteral("mm");
}

QString pt(qreal value)
{
    return QString::number(value, 'f', 2) + QStringLiteral("pt");
}

QString escaped(const QString &text)
{
    return text.toHtmlEscaped();
}

// Text as ODF: tabs, line breaks and runs of spaces as elements (ODF
// collapses whitespace in text). `afterSpace`: the previous character was a
// space, or this is the paragraph's start.
QString odfText(const QString &text, bool &afterSpace)
{
    QString out;
    int spaces = 0;
    const auto flushSpaces = [&] {
        if (spaces > 0)
            out += spaces == 1 ? QStringLiteral("<text:s/>") : QStringLiteral("<text:s text:c=\"%1\"/>").arg(spaces);
        spaces = 0;
    };
    for (const QChar ch : text) {
        if (ch == u' ') {
            if (afterSpace) {
                ++spaces;
            } else {
                out += ch;
                afterSpace = true;
            }
            continue;
        }
        flushSpaces();
        afterSpace = false;
        if (ch == u'\t') {
            out += QStringLiteral("<text:tab/>");
        } else if (ch == QChar::LineSeparator) {
            out += QStringLiteral("<text:line-break/>");
            afterSpace = true;
        } else if (ch == QChar::ObjectReplacementCharacter || ch.unicode() < 0x20) {
            // Objects Rune doesn't know (images) and control characters:
            // nothing ODF can hold here.
        } else {
            out += escaped(QString(ch));
        }
    }
    flushSpaces();
    return out;
}

QString alignmentValue(Qt::Alignment alignment)
{
    if (alignment & Qt::AlignHCenter)
        return QStringLiteral("center");
    if (alignment & Qt::AlignRight)
        return QStringLiteral("end");
    if (alignment & Qt::AlignJustify)
        return QStringLiteral("justify");
    return QStringLiteral("start");
}

QString borderStyle(QTextFrameFormat::BorderStyle style)
{
    switch (style) {
    case QTextFrameFormat::BorderStyle_None: return QStringLiteral("none");
    case QTextFrameFormat::BorderStyle_Dotted: return QStringLiteral("dotted");
    case QTextFrameFormat::BorderStyle_Dashed:
    case QTextFrameFormat::BorderStyle_DotDash:
    case QTextFrameFormat::BorderStyle_DotDotDash: return QStringLiteral("dashed");
    case QTextFrameFormat::BorderStyle_Double: return QStringLiteral("double");
    default: return QStringLiteral("solid");
    }
}

// A list level's label: its bullet or number format.
QString listLevelStyle(QTextListFormat::Style style, int level)
{
    const QString properties =
        QStringLiteral("<style:list-level-properties text:space-before=\"%1\" text:min-label-width=\"6.35mm\"/>")
            .arg(mm((level - 1) * 6.35));
    QString bullet;
    switch (style) {
    case QTextListFormat::ListDisc: bullet = QStringLiteral("●"); break;
    case QTextListFormat::ListCircle: bullet = QStringLiteral("○"); break;
    case QTextListFormat::ListSquare: bullet = QStringLiteral("■"); break;
    default: break;
    }
    if (!bullet.isEmpty())
        return QStringLiteral("<text:list-level-style-bullet text:level=\"%1\" text:bullet-char=\"%2\">%3"
                              "</text:list-level-style-bullet>")
            .arg(level).arg(bullet, properties);
    QString format = QStringLiteral("1");
    switch (style) {
    case QTextListFormat::ListLowerAlpha: format = QStringLiteral("a"); break;
    case QTextListFormat::ListUpperAlpha: format = QStringLiteral("A"); break;
    case QTextListFormat::ListLowerRoman: format = QStringLiteral("i"); break;
    case QTextListFormat::ListUpperRoman: format = QStringLiteral("I"); break;
    default: break;
    }
    return QStringLiteral("<text:list-level-style-number text:level=\"%1\" style:num-format=\"%2\" "
                          "style:num-suffix=\".\">%3</text:list-level-style-number>")
        .arg(level).arg(format, properties);
}

} // namespace

QString OdfTextWriter::namespaces()
{
    return QStringLiteral(
        "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
        "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
        "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
        "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
        "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" "
        "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" "
        "xmlns:xlink=\"http://www.w3.org/1999/xlink\"");
}

QString OdfTextWriter::headingStyleName(int level)
{
    return QStringLiteral("Heading_20_%1").arg(level);
}

QString OdfTextWriter::namedStyles()
{
    QString xml;
    for (int level = 1; level <= 6; ++level)
        xml += QStringLiteral("<style:style style:name=\"%1\" style:display-name=\"Heading %2\" style:family=\"paragraph\" "
                              "style:default-outline-level=\"%2\" style:class=\"text\"/>")
                   .arg(headingStyleName(level)).arg(level);
    return xml;
}

QString OdfTextWriter::write(const QTextDocument *doc)
{
    m_doc = doc;
    QTextCharFormat defaultFont;
    defaultFont.setFontFamilies({doc->defaultFont().family()});
    defaultFont.setFontPointSize(doc->defaultFont().pointSizeF());
    m_defaultText = textProperties(defaultFont);
    return writeFrameRange(doc->rootFrame());
}

QString OdfTextWriter::writeFrame(QTextFrame *frame)
{
    if (auto *table = qobject_cast<QTextTable *>(frame))
        return writeTable(table);
    return writeFrameRange(frame);
}

QString OdfTextWriter::writeFrameRange(QTextFrame *frame)
{
    return writeRange(frame->begin());
}

// Blocks and child frames from `it` to the end of its frame (or table
// cell), in order. List items are grouped into <text:list>s, nested by
// level: a deeper list sits in a list item of its own in the enclosing one.
QString OdfTextWriter::writeRange(QTextFrame::iterator it)
{
    QString xml;
    QList<OpenList> open;
    // Closes lists until `keep` are left open.
    const auto closeTo = [&](qsizetype keep) {
        while (open.size() > keep) {
            open.removeLast();
            xml += QStringLiteral("</text:list>");
            if (!open.isEmpty())
                xml += QStringLiteral("</text:list-item>");
        }
    };
    for (; !it.atEnd(); ++it) {
        if (QTextFrame *child = it.currentFrame()) {
            closeTo(0);
            xml += writeFrame(child);
            continue;
        }
        const QTextBlock block = it.currentBlock();
        QTextList *list = block.textList();
        if (!list) {
            closeTo(0);
            xml += writeBlock(block);
            continue;
        }
        const int level = std::max(1, list->format().indent());
        while (!open.isEmpty()
               && (open.constLast().level > level || (open.constLast().level == level && open.constLast().list != list)))
            closeTo(open.size() - 1);
        while (open.isEmpty() || open.constLast().level < level) {
            const int next = open.isEmpty() ? 1 : open.constLast().level + 1;
            if (!open.isEmpty())
                xml += QStringLiteral("<text:list-item>");
            xml += QStringLiteral("<text:list text:style-name=\"%1\"").arg(listStyle(list));
            // A list resumed after other content numbers on.
            if (next == level && m_listsWritten.contains(list))
                xml += QStringLiteral(" text:continue-numbering=\"true\"");
            xml += u'>';
            open.append({list, next});
        }
        m_listsWritten.insert(list);
        xml += QStringLiteral("<text:list-item>") + writeBlock(block) + QStringLiteral("</text:list-item>");
    }
    closeTo(0);
    return xml;
}

QString OdfTextWriter::writeBlock(const QTextBlock &block)
{
    const int heading = std::clamp(block.blockFormat().headingLevel(), 0, 6);
    const QString element = heading > 0 ? QStringLiteral("text:h") : QStringLiteral("text:p");
    QString xml = QStringLiteral("<%1 text:style-name=\"%2\"").arg(element, paragraphStyle(block));
    if (heading > 0)
        xml += QStringLiteral(" text:outline-level=\"%1\"").arg(heading);
    xml += u'>';

    bool afterSpace = true;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        const QTextCharFormat format = fragment.charFormat();
        QString content;
        if (const InlineObjects::Kind *kind = InlineObjects::forFormat(format)) {
            // One element per object character; adjacent objects share a
            // fragment, and so may text typed right after one (it takes the
            // object's format, but is text).
            for (const QChar ch : fragment.text()) {
                if (ch != QChar::ObjectReplacementCharacter) {
                    content += odfText(QString(ch), afterSpace);
                    continue;
                }
                content += kind->toOdf(format);
                afterSpace = false;
            }
        } else {
            content = odfText(fragment.text(), afterSpace);
        }
        if (content.isEmpty())
            continue;
        const QString style = textStyle(format);
        if (!style.isEmpty())
            content = QStringLiteral("<text:span text:style-name=\"%1\">%2</text:span>").arg(style, content);
        if (format.isAnchor() && !format.anchorHref().isEmpty())
            content = QStringLiteral("<text:a xlink:type=\"simple\" xlink:href=\"%1\">%2</text:a>")
                          .arg(escaped(format.anchorHref()), content);
        xml += content;
    }
    return xml + QStringLiteral("</%1>").arg(element);
}

QString OdfTextWriter::paragraphStyle(const QTextBlock &block)
{
    const QTextBlockFormat format = block.blockFormat();
    QString properties;
    if (format.hasProperty(QTextFormat::BlockAlignment))
        properties += QStringLiteral(" fo:text-align=\"%1\"").arg(alignmentValue(format.alignment()));
    const auto margin = [&](int property, const char *attribute) {
        if (format.hasProperty(property))
            properties += QStringLiteral(" fo:%1=\"%2\"").arg(QLatin1String(attribute), mm(format.doubleProperty(property) * kMmPerPx));
    };
    margin(QTextFormat::BlockTopMargin, "margin-top");
    margin(QTextFormat::BlockBottomMargin, "margin-bottom");
    margin(QTextFormat::BlockLeftMargin, "margin-left");
    margin(QTextFormat::BlockRightMargin, "margin-right");
    margin(QTextFormat::TextIndent, "text-indent");
    if (format.pageBreakPolicy() & QTextFormat::PageBreak_AlwaysBefore)
        properties += QStringLiteral(" fo:break-before=\"page\"");
    if (format.pageBreakPolicy() & QTextFormat::PageBreak_AlwaysAfter)
        properties += QStringLiteral(" fo:break-after=\"page\"");

    const int heading = std::clamp(format.headingLevel(), 0, 6);
    const QString parentName = heading > 0 ? headingStyleName(heading) : QString();
    const QString parent = parentName.isEmpty() ? QString() : QStringLiteral(" style:parent-style-name=\"%1\"").arg(parentName);
    QString &name = m_paragraphStyles[parent + properties];
    if (name.isEmpty()) {
        name = m_options.stylePrefix + QStringLiteral("P") + QString::number(m_paragraphStyles.size());
        m_styles += QStringLiteral("<style:style style:name=\"%1\" style:family=\"paragraph\"%2>"
                                   "<style:paragraph-properties%3>%4</style:paragraph-properties>"
                                   "<style:text-properties%5/></style:style>")
                        .arg(name, parent, properties, m_options.paragraphProperties, m_defaultText);
    }
    return name;
}

QString OdfTextWriter::textStyle(const QTextCharFormat &format)
{
    const QString properties = textProperties(format);
    if (properties.isEmpty())
        return {};
    QString &name = m_textStyles[properties];
    if (name.isEmpty()) {
        name = m_options.stylePrefix + QStringLiteral("T") + QString::number(m_textStyles.size());
        m_styles += QStringLiteral("<style:style style:name=\"%1\" style:family=\"text\">"
                                   "<style:text-properties%2/></style:style>")
                        .arg(name, properties);
    }
    return name;
}

// The <style:text-properties> attributes for what `format` sets itself.
QString OdfTextWriter::textProperties(const QTextCharFormat &format) const
{
    QString attrs;
    if (format.hasProperty(QTextFormat::FontWeight))
        attrs += format.fontWeight() >= QFont::Bold ? QStringLiteral(" fo:font-weight=\"bold\"")
                                                    : QStringLiteral(" fo:font-weight=\"normal\"");
    if (format.hasProperty(QTextFormat::FontItalic))
        attrs += format.fontItalic() ? QStringLiteral(" fo:font-style=\"italic\"") : QStringLiteral(" fo:font-style=\"normal\"");
    if (format.hasProperty(QTextFormat::FontUnderline) || format.hasProperty(QTextFormat::TextUnderlineStyle))
        attrs += format.fontUnderline()
            ? QStringLiteral(" style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" "
                             "style:text-underline-color=\"font-color\"")
            : QStringLiteral(" style:text-underline-style=\"none\"");
    if (format.hasProperty(QTextFormat::FontStrikeOut))
        attrs += format.fontStrikeOut() ? QStringLiteral(" style:text-line-through-style=\"solid\"")
                                        : QStringLiteral(" style:text-line-through-style=\"none\"");
    if (format.verticalAlignment() == QTextCharFormat::AlignSuperScript)
        attrs += QStringLiteral(" style:text-position=\"super 58%\"");
    else if (format.verticalAlignment() == QTextCharFormat::AlignSubScript)
        attrs += QStringLiteral(" style:text-position=\"sub 58%\"");
    QStringList families = format.fontFamilies().toStringList();
    if (families.isEmpty() && format.hasProperty(QTextFormat::FontFamily))
        families = {format.stringProperty(QTextFormat::FontFamily)};
    if (!families.isEmpty() && !families.constFirst().isEmpty())
        attrs += QStringLiteral(" fo:font-family=\"%1\"").arg(escaped(QLatin1Char('\'') + families.constFirst() + QLatin1Char('\'')));
    if (format.hasProperty(QTextFormat::FontPointSize)) {
        attrs += QStringLiteral(" fo:font-size=\"%1pt\"").arg(format.fontPointSize());
    } else if (format.hasProperty(QTextFormat::FontSizeAdjustment) && m_doc) {
        // Relative sizes (headings) as drawn: QTextFormat's scale for HTML
        // font sizes, of the default size.
        static const qreal scale[] = {0.7, 0.8, 1.0, 1.2, 1.5, 2.0, 2.4};
        const int step = std::clamp(format.intProperty(QTextFormat::FontSizeAdjustment) + 2, 0, 6);
        attrs += QStringLiteral(" fo:font-size=\"%1pt\"").arg(scale[step] * m_doc->defaultFont().pointSizeF());
    }
    if (format.hasProperty(QTextFormat::ForegroundBrush) && format.foreground().style() != Qt::NoBrush)
        attrs += QStringLiteral(" fo:color=\"%1\"").arg(format.foreground().color().name());
    if (format.hasProperty(QTextFormat::BackgroundBrush) && format.background().style() != Qt::NoBrush)
        attrs += QStringLiteral(" fo:background-color=\"%1\"").arg(format.background().color().name());
    return attrs;
}

// One list style per list: its own level as the list is, every other level
// as Rune would make it, for the enclosing lists a deeper one is written in.
QString OdfTextWriter::listStyle(QTextList *list)
{
    QString &name = m_listStyles[list];
    if (!name.isEmpty())
        return name;
    name = m_options.stylePrefix + QStringLiteral("L") + QString::number(m_listStyles.size());
    const QTextListFormat format = list->format();
    const Lists::Kind kind = Lists::kind(list);
    QString levels;
    for (int level = 1; level <= kListLevels; ++level)
        levels += listLevelStyle(level == format.indent() ? format.style() : Lists::style(kind, level), level);
    m_styles += QStringLiteral("<text:list-style style:name=\"%1\">%2</text:list-style>").arg(name, levels);
    return name;
}

QString OdfTextWriter::writeTable(QTextTable *table)
{
    const QTextTableFormat format = table->format();
    const QString name = m_options.stylePrefix + QStringLiteral("Table") + QString::number(++m_tables);

    // The table: its width (relative to the text area, or fixed) and borders.
    QString tableProperties;
    const QTextLength width = format.width();
    if (width.type() == QTextLength::PercentageLength) {
        const qreal percent = std::clamp<qreal>(width.rawValue(), 1, 100);
        tableProperties += QStringLiteral(" style:width=\"%1\" style:rel-width=\"%2%\" table:align=\"%3\"")
                               .arg(mm(m_options.textWidthMm * percent / 100)).arg(percent)
                               .arg(percent >= 100 ? QStringLiteral("margins") : QStringLiteral("left"));
    } else if (width.type() == QTextLength::FixedLength) {
        tableProperties += QStringLiteral(" style:width=\"%1\" table:align=\"left\"").arg(mm(width.rawValue() * kMmPerPx));
    } else {
        tableProperties += QStringLiteral(" table:align=\"margins\"");
    }
    tableProperties += format.borderCollapse() ? QStringLiteral(" table:border-model=\"collapsing\"")
                                               : QStringLiteral(" table:border-model=\"separating\"");
    m_styles += QStringLiteral("<style:style style:name=\"%1\" style:family=\"table\"><style:table-properties%2/></style:style>")
                    .arg(name, tableProperties);

    // Cells: the table's border and padding; a background of their own.
    const QString border = format.border() > 0 && format.borderStyle() != QTextFrameFormat::BorderStyle_None
        ? QStringLiteral("%1 %2 %3").arg(pt(format.border() * kPtPerPx), borderStyle(format.borderStyle()),
                                         format.borderBrush().style() != Qt::NoBrush ? format.borderBrush().color().name()
                                                                                     : QStringLiteral("#808080"))
        : QStringLiteral("none");
    QHash<QString, QString> cellStyles; // background ("" none) -> name
    const auto cellStyle = [&](const QTextTableCellFormat &cellFormat) {
        const QString background = cellFormat.background().style() != Qt::NoBrush ? cellFormat.background().color().name()
                                                                                    : QString();
        QString &cellName = cellStyles[background];
        if (cellName.isEmpty()) {
            cellName = name + QStringLiteral(".C") + QString::number(cellStyles.size());
            QString properties = QStringLiteral(" fo:border=\"%1\" fo:padding=\"%2\"")
                                     .arg(border, pt(std::max<qreal>(0, format.cellPadding()) * kPtPerPx));
            if (!background.isEmpty())
                properties += QStringLiteral(" fo:background-color=\"%1\"").arg(background);
            m_styles += QStringLiteral("<style:style style:name=\"%1\" style:family=\"table-cell\">"
                                       "<style:table-cell-properties%2/></style:style>")
                            .arg(cellName, properties);
        }
        return cellName;
    };

    QString xml = QStringLiteral("<table:table table:name=\"%1\" table:style-name=\"%1\">"
                                 "<table:table-column table:number-columns-repeated=\"%2\"/>")
                      .arg(name).arg(table->columns());
    for (int r = 0; r < table->rows(); ++r) {
        xml += QStringLiteral("<table:table-row>");
        for (int c = 0; c < table->columns(); ++c) {
            const QTextTableCell cell = table->cellAt(r, c);
            if (cell.row() != r || cell.column() != c) {
                xml += QStringLiteral("<table:covered-table-cell/>");
                continue;
            }
            xml += QStringLiteral("<table:table-cell table:style-name=\"%1\" office:value-type=\"string\"")
                       .arg(cellStyle(cell.format().toTableCellFormat()));
            if (cell.columnSpan() > 1)
                xml += QStringLiteral(" table:number-columns-spanned=\"%1\"").arg(cell.columnSpan());
            if (cell.rowSpan() > 1)
                xml += QStringLiteral(" table:number-rows-spanned=\"%1\"").arg(cell.rowSpan());
            xml += u'>';
            // The cell's blocks, lists and nested tables, as a frame's are.
            const QString content = writeRange(cell.begin());
            xml += content.isEmpty() ? QStringLiteral("<text:p/>") : content;
            xml += QStringLiteral("</table:table-cell>");
        }
        xml += QStringLiteral("</table:table-row>");
    }
    return xml + QStringLiteral("</table:table>");
}
