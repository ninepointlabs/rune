#include "PageParts.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>
#include <QTextBlock>
#include <QTextDocument>



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

InlineObjects::Kind PageParts::fieldKind()
{
    const QString textNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:text:1.0");
    InlineObjects::Kind kind;
    kind.objectType = kFieldObject;
    kind.allowedIn = {DocumentParts::Kind::PageMargin};
    kind.install = [](QTextDocument *doc) { PageFieldHandler::install(doc); };
    kind.toOdf = [](const QTextFormat &format) {
        return format.intProperty(kFieldKind) == int(Field::PageNumber)
            ? QStringLiteral("<text:page-number text:select-page=\"current\">1</text:page-number>")
            : QStringLiteral("<text:page-count>1</text:page-count>");
    };
    kind.odfElements = {{textNs, QStringLiteral("page-number")}, {textNs, QStringLiteral("page-count")}};
    kind.fromOdf = [](const QString &name, const QTextCharFormat &base) {
        return fieldFormat(base, name == QLatin1String("page-number") ? Field::PageNumber : Field::PageCount);
    };
    return kind;
}

bool PageParts::isEmpty(const QTextDocument *doc)
{
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (!b.text().trimmed().isEmpty())
            return false;
    return true;
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
