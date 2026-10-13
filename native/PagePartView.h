#pragma once

// PagePartView: a page's header or footer, as the page shows it (its own
// page number in the fields), drawn from DocumentController's header or
// footer document. Not editable; the page view puts a TextEdit over it
// while it is edited.
//
//   PagePartView { documentController: controller; part: "header"; page: index + 1; color: ... }

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>

#include "DocumentController.h"

class PagePartView : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(DocumentController *documentController READ controller WRITE setController NOTIFY controllerChanged)
    // "header" or "footer" (see DocumentController::partDocument()).
    Q_PROPERTY(QString part READ part WRITE setPart NOTIFY partChanged)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    // Only the fields: over the TextEdit of the header or footer being
    // edited (see HeaderFooter::setEditing()).
    Q_PROPERTY(bool fieldsOnly READ fieldsOnly WRITE setFieldsOnly NOTIFY fieldsOnlyChanged)

public:
    explicit PagePartView(QQuickItem *parent = nullptr);

    DocumentController *controller() const { return m_controller; }
    void setController(DocumentController *controller);
    QString part() const { return m_part; }
    void setPart(const QString &part);
    int page() const { return m_page; }
    void setPage(int page);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    bool fieldsOnly() const { return m_fieldsOnly; }
    void setFieldsOnly(bool fieldsOnly);

    void paint(QPainter *painter) override;

signals:
    void controllerChanged();
    void partChanged();
    void pageChanged();
    void colorChanged();
    void fieldsOnlyChanged();

private:
    QPointer<DocumentController> m_controller;
    QString m_part;
    int m_page = 1;
    QColor m_color = Qt::black;
    bool m_fieldsOnly = false;
};
