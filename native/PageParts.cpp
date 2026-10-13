#include "PageParts.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QTextBlock>
#include <QTextDocument>

namespace {

QString mm(qreal value)
{
    return QString::number(value, 'f', 2) + QStringLiteral("mm");
}

QString escaped(const QString &text)
{
    return text.toHtmlEscaped();
}

// The <style:text-properties> attributes for what `format` sets.
QString textProperties(const QTextCharFormat &format)
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
    const QStringList families = format.fontFamilies().toStringList();
    if (!families.isEmpty())
        attrs += QStringLiteral(" fo:font-family=\"%1\"").arg(escaped(QLatin1Char('\'') + families.first() + QLatin1Char('\'')));
    if (format.hasProperty(QTextFormat::FontPointSize))
        attrs += QStringLiteral(" fo:font-size=\"%1pt\"").arg(format.fontPointSize());
    if (format.hasProperty(QTextFormat::ForegroundBrush) && format.foreground().style() != Qt::NoBrush)
        attrs += QStringLiteral(" fo:color=\"%1\"").arg(format.foreground().color().name());
    return attrs;
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
            // Images and control characters: nothing ODF can hold here.
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

} // namespace

QTextCharFormat PageParts::fieldFormat(const QTextCharFormat &base, Field field)
{
    QTextCharFormat format = base;
    format.setObjectType(kFieldObject);
    format.setProperty(kFieldKind, int(field));
    // Sit on the baseline like the text around it.
    format.setVerticalAlignment(QTextCharFormat::AlignBaseline);
    return format;
}

bool PageParts::isField(const QTextFormat &format)
{
    return format.objectType() == kFieldObject;
}

bool PageParts::isEmpty(const QTextDocument *doc)
{
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (!b.text().trimmed().isEmpty())
            return false;
    return true;
}

QString PageParts::toOdf(const QTextDocument *doc, const QString &stylePrefix, qreal textWidthMm,
                         QString &automaticStyles)
{
    if (isEmpty(doc))
        return {};
    // Automatic styles shared by every paragraph or span that needs the same.
    QHash<QString, QString> paragraphStyles, textStyles;
    const QString tabStops = QStringLiteral("<style:tab-stops>"
                                            "<style:tab-stop style:position=\"%1\" style:type=\"center\"/>"
                                            "<style:tab-stop style:position=\"%2\" style:type=\"right\"/>"
                                            "</style:tab-stops>")
                                 .arg(mm(textWidthMm / 2), mm(textWidthMm));
    const auto paragraphStyle = [&](const QString &align) {
        QString &name = paragraphStyles[align];
        if (name.isEmpty()) {
            name = stylePrefix + QStringLiteral("P") + QString::number(paragraphStyles.size());
            automaticStyles += QStringLiteral("<style:style style:name=\"%1\" style:family=\"paragraph\">"
                                              "<style:paragraph-properties fo:text-align=\"%2\">%3</style:paragraph-properties>"
                                              "</style:style>")
                                   .arg(name, align, tabStops);
        }
        return name;
    };
    const auto textStyle = [&](const QString &properties) {
        QString &name = textStyles[properties];
        if (name.isEmpty()) {
            name = stylePrefix + QStringLiteral("T") + QString::number(textStyles.size());
            automaticStyles += QStringLiteral("<style:style style:name=\"%1\" style:family=\"text\">"
                                              "<style:text-properties%2/></style:style>")
                                   .arg(name, properties);
        }
        return name;
    };

    // Every block in order, table cells' included (written as paragraphs).
    QString xml;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        xml += QStringLiteral("<text:p text:style-name=\"%1\">").arg(paragraphStyle(alignmentValue(block.blockFormat().alignment())));
        bool afterSpace = true;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat format = fragment.charFormat();
            QString content;
            if (isField(format)) {
                // A field per object character; adjacent fields share a
                // fragment, and so may text typed right after one (it takes
                // the field's format, but is text).
                const bool number = format.intProperty(kFieldKind) == int(Field::PageNumber);
                for (const QChar ch : fragment.text()) {
                    if (ch != QChar::ObjectReplacementCharacter) {
                        content += odfText(QString(ch), afterSpace);
                        continue;
                    }
                    content += number ? QStringLiteral("<text:page-number text:select-page=\"current\">1</text:page-number>")
                                      : QStringLiteral("<text:page-count>1</text:page-count>");
                    afterSpace = false;
                }
            } else {
                content = odfText(fragment.text(), afterSpace);
            }
            if (content.isEmpty())
                continue;
            const QString properties = textProperties(format);
            xml += properties.isEmpty() ? content
                                        : QStringLiteral("<text:span text:style-name=\"%1\">%2</text:span>")
                                              .arg(textStyle(properties), content);
        }
        xml += QStringLiteral("</text:p>");
    }
    return xml;
}

PageFieldHandler *PageFieldHandler::install(QTextDocument *doc)
{
    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    if (auto *registered = dynamic_cast<PageFieldHandler *>(layout->handlerForObject(PageParts::kFieldObject)))
        return registered;
    auto *handler = doc->findChild<PageFieldHandler *>(Qt::FindDirectChildrenOnly);
    if (!handler)
        handler = new PageFieldHandler(doc);
    layout->registerHandler(PageParts::kFieldObject, handler);
    return handler;
}

QFont PageFieldHandler::fontFor(QTextDocument *doc, const QTextFormat &format) const
{
    return format.toCharFormat().font().resolve(doc->defaultFont());
}

QSizeF PageFieldHandler::intrinsicSize(QTextDocument *doc, int, const QTextFormat &format)
{
    const QFontMetricsF metrics(fontFor(doc, format));
    const int digits = int(QString::number(std::max(page, pageCount)).size());
    return {metrics.horizontalAdvance(QString(digits, u'0')), metrics.ascent() + metrics.descent()};
}

void PageFieldHandler::drawObject(QPainter *painter, const QRectF &rect, QTextDocument *doc, int,
                                  const QTextFormat &format)
{
    const QFont font = fontFor(doc, format);
    const QTextCharFormat charFormat = format.toCharFormat();
    const bool number = format.intProperty(PageParts::kFieldKind) == int(PageParts::Field::PageNumber);
    painter->save();
    painter->setFont(font);
    painter->setPen(charFormat.hasProperty(QTextFormat::ForegroundBrush) && charFormat.foreground().style() != Qt::NoBrush
                        ? charFormat.foreground().color() : color);
    // Centred in the room of the widest number, so a narrower one leaves
    // the same space either side.
    const QString text = QString::number(number ? page : pageCount);
    const QFontMetricsF metrics(font);
    painter->drawText(QPointF(rect.left() + (rect.width() - metrics.horizontalAdvance(text)) / 2, rect.top() + metrics.ascent()),
                      text);
    painter->restore();
}
