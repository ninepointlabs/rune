#include "PagePartView.h"

PagePartView::PagePartView(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

void PagePartView::setController(DocumentController *controller)
{
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller)
        connect(m_controller, &DocumentController::pagePartsChanged, this, [this] { update(); });
    update();
    emit controllerChanged();
}

void PagePartView::setPart(const QString &part)
{
    if (m_part == part)
        return;
    m_part = part;
    update();
    emit partChanged();
}

void PagePartView::setPage(int page)
{
    if (m_page == page)
        return;
    m_page = page;
    update();
    emit pageChanged();
}

void PagePartView::setColor(const QColor &color)
{
    if (m_color == color)
        return;
    m_color = color;
    update();
    emit colorChanged();
}

void PagePartView::paint(QPainter *painter)
{
    if (m_controller)
        m_controller->paintPart(painter, m_part, m_page, m_color);
}
