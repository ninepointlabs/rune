#include "HeaderFooter.h"

#include "PageParts.h"

#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QQuickTextDocument>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextOption>

#include <algorithm>

namespace {
constexpr qreal kMmPerPx = 25.4 / 96.0;
// The least room above and below a header or footer, in its margin.
constexpr qreal kPartPaddingMm = 3.0;
} // namespace

HeaderFooter::HeaderFooter(const QFont &font, QObject *parent)
    : QObject(parent), m_header(new QTextDocument(this)), m_footer(new QTextDocument(this))
{
    for (QTextDocument *part : {m_header, m_footer}) {
        part->setDefaultFont(font);
        part->setDocumentMargin(0);
        connect(part, &QTextDocument::contentsChanged, this, &HeaderFooter::changed);
        connect(part->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
                &HeaderFooter::changed);
        PageFieldHandler::install(part);
    }
}

bool HeaderFooter::has(bool footer) const
{
    return !PageParts::isEmpty(document(footer));
}

qreal HeaderFooter::heightPx(bool footer) const
{
    return document(footer)->documentLayout()->documentSize().height();
}

qreal HeaderFooter::headerYPx(const PageLayout &layout) const
{
    return std::max<qreal>(0, (layout.marginsPx().top() - heightPx(false)) / 2);
}

qreal HeaderFooter::footerYPx(const PageLayout &layout) const
{
    const qreal margin = layout.marginsPx().bottom();
    return layout.sizePx().height() - margin + std::max<qreal>(0, (margin - heightPx(true)) / 2);
}

void HeaderFooter::install(bool footer, QQuickTextDocument *qmlDocument, const PageLayout &layout)
{
    QTextDocument *part = document(footer);
    if (!qmlDocument)
        return;
    qmlDocument->setTextDocument(part);
    // The TextEdit may have given the document a layout of its own.
    connect(part->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            &HeaderFooter::changed, Qt::UniqueConnection);
    PageFieldHandler *handler = PageFieldHandler::install(part);
    handler->pageCount = m_pageCount;
    handler->page = m_editPage;
    handler->color = m_editColor;
    // ... and its own margin and width.
    part->setDocumentMargin(0);
    syncWidths(layout);
    emit changed();
}

int HeaderFooter::insertPageField(bool footer, int position, bool count)
{
    QTextDocument *part = document(footer);
    QTextCursor cursor(part);
    cursor.setPosition(std::clamp(position, 0, part->characterCount() - 1));
    // Formatted like the text before it, unless that is a field itself.
    QTextCharFormat base = cursor.charFormat();
    base.clearProperty(QTextFormat::ObjectType);
    base.clearProperty(PageParts::kFieldKind);
    base.clearProperty(QTextFormat::TextVerticalAlignment);
    cursor.insertText(QString(QChar::ObjectReplacementCharacter),
                      PageParts::fieldFormat(base, count ? PageParts::Field::PageCount : PageParts::Field::PageNumber));
    return cursor.position();
}

void HeaderFooter::setEditing(int page, const QColor &color)
{
    m_editPage = std::max(1, page);
    m_editColor = color;
    for (QTextDocument *part : {m_header, m_footer}) {
        PageFieldHandler *handler = PageFieldHandler::install(part);
        handler->page = m_editPage;
        handler->color = m_editColor;
        // Draws the fields again, with the new number.
        part->markContentsDirty(0, part->characterCount());
    }
}

void HeaderFooter::setPageCount(int pages)
{
    m_pageCount = pages;
    // Fields take the room of the widest page number: lay them out again.
    for (QTextDocument *part : {m_header, m_footer}) {
        PageFieldHandler::install(part)->pageCount = pages;
        part->markContentsDirty(0, part->characterCount());
    }
}

void HeaderFooter::syncWidths(const PageLayout &layout)
{
    const QSizeF size = layout.sizePx();
    const QMarginsF margins = layout.marginsPx();
    const qreal width = size.width() - margins.left() - margins.right();
    for (QTextDocument *part : {m_header, m_footer}) {
        if (qFuzzyCompare(part->textWidth(), width) && !part->defaultTextOption().tabs().isEmpty())
            continue;
        part->setTextWidth(width);
        // Tab stops as in a word processor's header: centre, then right.
        QTextOption option = part->defaultTextOption();
        option.setTabs({QTextOption::Tab(width / 2, QTextOption::CenterTab),
                        QTextOption::Tab(width - 0.5, QTextOption::RightTab)});
        part->setDefaultTextOption(option);
    }
}

PageLayout HeaderFooter::withRoom(PageLayout layout) const
{
    if (has(false))
        layout.topMm = std::max(layout.topMm, heightPx(false) * kMmPerPx + 2 * kPartPaddingMm);
    if (has(true))
        layout.bottomMm = std::max(layout.bottomMm, heightPx(true) * kMmPerPx + 2 * kPartPaddingMm);
    return layout;
}

void HeaderFooter::paint(QPainter *painter, bool footer, int page, const QColor &color)
{
    QTextDocument *part = document(footer);
    PageFieldHandler *handler = PageFieldHandler::install(part);
    handler->page = page;
    handler->color = color;
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, color);
    part->documentLayout()->draw(painter, context);
    // Back to what the TextEdits show.
    handler->page = m_editPage;
    handler->color = m_editColor;
}

void HeaderFooter::clear()
{
    for (QTextDocument *part : {m_header, m_footer}) {
        part->clear();
        part->clearUndoRedoStacks();
        part->setModified(false);
    }
}

bool HeaderFooter::isModified() const
{
    return m_header->isModified() || m_footer->isModified();
}

void HeaderFooter::setModified(bool modified)
{
    m_header->setModified(modified);
    m_footer->setModified(modified);
}

QByteArray HeaderFooter::odfStyles(PageLayout layout) const
{
    const qreal textWidthMm = layout.widthMm - layout.leftMm - layout.rightMm;
    // Where they are drawn: centred in their margins.
    const qreal headerY = headerYPx(layout);
    const qreal footerY = footerYPx(layout);
    layout.headerMm = headerY * kMmPerPx;
    layout.footerMm = (layout.sizePx().height() - footerY - heightPx(true)) * kMmPerPx;
    PageLayout::OdfParts parts;
    parts.header = PageParts::toOdf(m_header, QStringLiteral("Header"), textWidthMm, parts.automaticStyles);
    parts.footer = PageParts::toOdf(m_footer, QStringLiteral("Footer"), textWidthMm, parts.automaticStyles);
    return layout.odfStyles(parts);
}
