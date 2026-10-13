#pragma once

// PagePartView: a page's header or footer, as the page shows it (its own
// page number in the fields), drawn from DocumentController's header or
// footer document. Not editable; the page view puts a TextEdit over it
// while it is edited.
//
//   PagePartView { documentController: controller; footer: false; page: index + 1; color: ... }

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>

#include "DocumentController.h"

class PagePartView : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(DocumentController *documentController READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(bool footer READ isFooter WRITE setFooter NOTIFY footerChanged)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)

public:
    explicit PagePartView(QQuickItem *parent = nullptr);

    DocumentController *controller() const { return m_controller; }
    void setController(DocumentController *controller);
    bool isFooter() const { return m_footer; }
    void setFooter(bool footer);
    int page() const { return m_page; }
    void setPage(int page);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);

    void paint(QPainter *painter) override;

signals:
    void controllerChanged();
    void footerChanged();
    void pageChanged();
    void colorChanged();

private:
    QPointer<DocumentController> m_controller;
    bool m_footer = false;
    int m_page = 1;
    QColor m_color = Qt::black;
};
