#include "Citations.h"

#include "FormatIds.h"
#include "Outline.h"
#include "Sources.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

namespace Citations {

namespace {

// In pixels at the editor's 96 per inch: the TextEdit draws objects into
// an image of its own resolution, where a point size would come out at
// another size than the text around it is laid out at.
QFont fontFor(QTextDocument *doc, const QTextFormat &format)
{
    QFont font = format.toCharFormat().font().resolve(doc->defaultFont());
    if (font.pointSizeF() > 0)
        font.setPixelSize(qRound(font.pointSizeF() * 96.0 / 72.0));
    return font;
}

// Every citation character, in document order, as (position, format).
template<typename Visit>
void forEachCitation(const QTextDocument *doc, Visit visit)
{
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!isCitation(fragment.charFormat()))
                continue;
            const QString text = fragment.text();
            for (qsizetype i = 0; i < text.size(); ++i)
                if (text.at(i) == QChar::ObjectReplacementCharacter)
                    visit(fragment.position() + int(i), fragment.charFormat());
        }
}

QString escaped(const QString &text)
{
    return text.toHtmlEscaped();
}

} // namespace

QTextCharFormat format(const QStringList &ids, const QString &locator, const QTextCharFormat &base)
{
    QTextCharFormat f = base;
    f.setObjectType(FormatIds::kCitationObject);
    f.setProperty(FormatIds::kCitationIds, ids);
    if (locator.trimmed().isEmpty())
        f.clearProperty(FormatIds::kCitationLocator);
    else
        f.setProperty(FormatIds::kCitationLocator, locator.trimmed());
    f.clearProperty(FormatIds::kCitationSaved);
    f.clearProperty(FormatIds::kCitationSavedText);
    f.setVerticalAlignment(QTextCharFormat::AlignBaseline);
    return f;
}

bool isCitation(const QTextFormat &format)
{
    return format.objectType() == FormatIds::kCitationObject;
}

QStringList ids(const QTextFormat &format)
{
    return format.property(FormatIds::kCitationIds).toStringList();
}

QString locator(const QTextFormat &format)
{
    return format.stringProperty(FormatIds::kCitationLocator);
}

QString toMarkdown(const QStringList &ids, const QString &locator)
{
    QStringList keys;
    for (const QString &id : ids)
        keys << QLatin1Char('@') + id;
    QString out = QLatin1Char('[') + keys.join(QStringLiteral("; "));
    if (!locator.isEmpty())
        out += QStringLiteral(", ") + locator;
    return out + QLatin1Char(']');
}

QList<MarkdownCitation> findInMarkdown(const QString &text)
{
    // [@key], [@a; @b], [@key, p. 12]: keys as Pandoc's (letters, digits
    // and internal punctuation).
    static const QRegularExpression citation(
        QStringLiteral(R"(\[(@[\w][\w:.#$%&+?<>~/-]*(?:\s*;\s*@[\w][\w:.#$%&+?<>~/-]*)*)\s*(?:,\s*([^\]\[]*?))?\s*\])"));
    QList<MarkdownCitation> out;
    for (auto it = citation.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        MarkdownCitation c;
        c.start = m.capturedStart();
        c.length = m.capturedLength();
        for (QString key : m.captured(1).split(u';', Qt::SkipEmptyParts)) {
            key = key.trimmed();
            // A key may end in punctuation Pandoc leaves out of it.
            while (key.size() > 1 && QStringView(u".:,").contains(key.back()))
                key.chop(1);
            c.ids << key.mid(1);
        }
        c.locator = m.captured(2).trimmed();
        out.append(c);
    }
    return out;
}

InlineObjects::Kind kind()
{
    InlineObjects::Kind kind;
    kind.objectType = FormatIds::kCitationObject;
    kind.allowedIn = {DocumentParts::Kind::Body};
    kind.install = [](QTextDocument *doc) {
        Engine::attach(doc);
        Handler::install(doc);
    };
    kind.toOdf = [](const QTextFormat &format, InlineObjects::OdfContext &context) {
        const Engine *engine = context.doc ? Engine::of(context.doc) : nullptr;
        const QString text = engine ? engine->render(format) : toMarkdown(ids(format), locator(format));
        if (!context.data)
            return escaped(text);
        QJsonObject saved = context.data->value(kSavedStore).toObject();
        const QString number = QString::number(saved.size() + 1);
        QJsonObject entry{{QStringLiteral("ids"), QJsonArray::fromStringList(ids(format))}};
        if (!locator(format).isEmpty())
            entry.insert(QStringLiteral("locator"), locator(format));
        saved.insert(number, entry);
        context.data->insert(kSavedStore, saved);
        const QString name = kBookmarkPrefix + number;
        return QStringLiteral("<text:bookmark-start text:name=\"%1\"/>%2<text:bookmark-end text:name=\"%1\"/>")
            .arg(name, escaped(text));
    };
    kind.formatProperties = {FormatIds::kCitationIds, FormatIds::kCitationLocator, FormatIds::kCitationSaved,
                             FormatIds::kCitationSavedText, QTextFormat::TextVerticalAlignment};
    kind.odfRangePrefix = kBookmarkPrefix;
    kind.fromOdfRange = [](const QString &name, const QString &text, const QTextCharFormat &base) {
        QTextCharFormat f = base;
        f.setObjectType(FormatIds::kCitationObject);
        f.setProperty(FormatIds::kCitationSaved, name.mid(kBookmarkPrefix.size()));
        f.setProperty(FormatIds::kCitationSavedText, text);
        f.setVerticalAlignment(QTextCharFormat::AlignBaseline);
        return f;
    };
    kind.toMarkdown = [](const QTextFormat &format) { return toMarkdown(ids(format), locator(format)); };
    return kind;
}

// --- Engine ----------------------------------------------------------------

Engine::Engine(QTextDocument *doc) : QObject(doc), m_doc(doc) {}

Engine *Engine::of(const QTextDocument *doc)
{
    return doc ? doc->findChild<Engine *>(Qt::FindDirectChildrenOnly) : nullptr;
}

Engine *Engine::attach(QTextDocument *doc)
{
    if (Engine *engine = of(doc))
        return engine;
    return new Engine(doc);
}

void Engine::setSources(const QJsonArray &sources)
{
    if (m_sources == sources)
        return;
    m_sources = sources;
    redraw();
}

void Engine::setStyle(CitationStyles::Style style)
{
    if (m_style == style)
        return;
    m_style = style;
    redraw();
}

void Engine::setColor(const QColor &color)
{
    if (m_color == color)
        return;
    m_color = color;
    redraw();
}

void Engine::refresh()
{
    QStringList order;
    forEachCitation(m_doc, [&](int, const QTextCharFormat &format) {
        for (const QString &id : ids(format))
            if (!order.contains(id))
                order << id;
    });
    if (order == m_citedOrder)
        return;
    m_citedOrder = order;
    if (m_style == CitationStyles::Style::Ieee)
        redraw();
}

void Engine::redraw()
{
    // Citations' widths change with their text: lay them out again.
    forEachCitation(m_doc, [this](int position, const QTextCharFormat &) { m_doc->markContentsDirty(position, 1); });
}

QString Engine::render(const QTextFormat &format) const
{
    const QStringList cited = ids(format);
    if (cited.isEmpty())
        return format.stringProperty(FormatIds::kCitationSavedText);
    QList<QJsonObject> items;
    QList<int> numbers;
    QStringList missing;
    for (const QString &id : cited) {
        const QJsonObject item = Sources::find(m_sources, id);
        if (item.isEmpty()) {
            missing << id;
            continue;
        }
        items << item;
        const qsizetype n = m_citedOrder.indexOf(id);
        numbers << int(n < 0 ? m_citedOrder.size() + 1 : n + 1);
    }
    if (items.isEmpty())
        return QStringLiteral("[missing source: %1]").arg(missing.join(QStringLiteral(", ")));
    return CitationStyles::inText(m_style, items, locator(format), numbers);
}

QList<QJsonObject> Engine::bibliographySources() const
{
    QList<QJsonObject> items;
    for (const QString &id : m_citedOrder) {
        const QJsonObject item = Sources::find(m_sources, id);
        if (!item.isEmpty())
            items << item;
    }
    if (!CitationStyles::ordersByCitation(m_style))
        std::stable_sort(items.begin(), items.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return CitationStyles::sortKey(a) < CitationStyles::sortKey(b);
        });
    return items;
}

// --- Drawing ---------------------------------------------------------------

void Handler::install(QTextDocument *doc)
{
    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    if (dynamic_cast<Handler *>(layout->handlerForObject(FormatIds::kCitationObject)))
        return;
    auto *handler = doc->findChild<Handler *>(Qt::FindDirectChildrenOnly);
    if (!handler)
        handler = new Handler(doc);
    layout->registerHandler(FormatIds::kCitationObject, handler);
}

QSizeF Handler::intrinsicSize(QTextDocument *doc, int, const QTextFormat &format)
{
    const Engine *engine = Engine::of(doc);
    const QFontMetricsF metrics(fontFor(doc, format));
    const QString text = engine ? engine->render(format) : QStringLiteral("[?]");
    return {metrics.horizontalAdvance(text), metrics.ascent() + metrics.descent()};
}

void Handler::drawObject(QPainter *painter, const QRectF &rect, QTextDocument *doc, int, const QTextFormat &format)
{
    const Engine *engine = Engine::of(doc);
    const QFont font = fontFor(doc, format);
    const QTextCharFormat charFormat = format.toCharFormat();
    painter->save();
    painter->setFont(font);
    if (charFormat.hasProperty(QTextFormat::ForegroundBrush) && charFormat.foreground().style() != Qt::NoBrush)
        painter->setPen(charFormat.foreground().color());
    else if (engine)
        painter->setPen(engine->color());
    painter->drawText(QPointF(rect.left(), rect.top() + QFontMetricsF(font).ascent()),
                      engine ? engine->render(format) : QStringLiteral("[?]"));
    painter->restore();
}

// --- Bibliography ----------------------------------------------------------

bool hasBibliography(const QTextDocument *doc)
{
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (b.blockFormat().hasProperty(FormatIds::kBibliography))
            return true;
    return false;
}

bool writeBibliography(QTextCursor cursor, bool insert, bool joinPrevious)
{
    QTextDocument *doc = cursor.document();
    const Engine *engine = Engine::of(doc);
    if (!engine)
        return false;
    QTextBlock first, last;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (b.blockFormat().hasProperty(FormatIds::kBibliography)) {
            if (!first.isValid())
                first = b;
            last = b;
        }
    if (!first.isValid() && !insert)
        return false;

    const CitationStyles::Style style = engine->style();
    const QList<QJsonObject> items = engine->bibliographySources();
    QTextBlockFormat heading;
    heading.setHeadingLevel(1);
    heading.setProperty(FormatIds::kBibliography, 1);
    QTextBlockFormat entry;
    entry.setProperty(FormatIds::kBibliography, 2);
    if (style != CitationStyles::Style::Ieee) {
        // A hanging indent of half an inch.
        entry.setLeftMargin(48);
        entry.setTextIndent(-48);
    }
    QTextCharFormat italic;
    italic.setFontItalic(true);

    // Unchanged: leave it be (and the history alone).
    if (first.isValid()) {
        QStringList now;
        for (QTextBlock b = first; b.isValid(); b = b.next()) {
            now << b.text();
            if (b == last)
                break;
        }
        QStringList wanted{CitationStyles::bibliographyTitle(style)};
        for (qsizetype i = 0; i < items.size(); ++i)
            wanted << CitationStyles::plain(CitationStyles::entry(style, items.at(i), int(i + 1)));
        if (now == wanted)
            return false;
    }

    if (joinPrevious)
        cursor.joinPreviousEditBlock();
    else
        cursor.beginEditBlock();
    if (first.isValid()) {
        cursor.setPosition(first.position());
        cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    } else if (!cursor.block().text().isEmpty()) {
        cursor.clearSelection();
        cursor.movePosition(QTextCursor::EndOfBlock);
        cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    }
    cursor.setBlockFormat(heading);
    cursor.setBlockCharFormat(Outline::headingLook(1));
    cursor.insertText(CitationStyles::bibliographyTitle(style), Outline::headingLook(1));
    for (qsizetype i = 0; i < items.size(); ++i) {
        cursor.insertBlock(entry, QTextCharFormat());
        for (const CitationStyles::Run &run : CitationStyles::entry(style, items.at(i), int(i + 1)))
            cursor.insertText(run.text, run.italic ? italic : QTextCharFormat());
    }
    cursor.endEditBlock();
    return true;
}

void resolveSaved(QTextDocument *doc, const QJsonObject &saved)
{
    QList<QPair<int, QTextCharFormat>> found;
    forEachCitation(doc, [&](int position, const QTextCharFormat &format) {
        if (format.hasProperty(FormatIds::kCitationSaved))
            found.append({position, format});
    });
    // Back to front: replacing one with its text moves the ones after it.
    for (auto it = found.crbegin(); it != found.crend(); ++it) {
        const QTextCharFormat &old = it->second;
        const QJsonObject entry = saved.value(old.stringProperty(FormatIds::kCitationSaved)).toObject();
        QStringList cited;
        for (const QJsonValue &id : entry.value(QStringLiteral("ids")).toArray())
            cited << id.toString();
        QTextCursor cursor(doc);
        cursor.setPosition(it->first);
        cursor.setPosition(it->first + 1, QTextCursor::KeepAnchor);
        QTextCharFormat base = old;
        base.clearProperty(QTextFormat::ObjectType);
        base.clearProperty(FormatIds::kCitationSaved);
        base.clearProperty(FormatIds::kCitationSavedText);
        if (cited.isEmpty())
            cursor.insertText(old.stringProperty(FormatIds::kCitationSavedText), base);
        else
            cursor.setCharFormat(format(cited, entry.value(QStringLiteral("locator")).toString(), base));
    }
}

} // namespace Citations
