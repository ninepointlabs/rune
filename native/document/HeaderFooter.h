#pragma once

// HeaderFooter: the header and footer, documents of their own (parts
// "header" and "footer" of DocumentParts, kind PageMargin), the same on
// every page but for their page number fields (PageParts).
//
// PagePartView draws them on each page (paint()); a TextEdit each edits them
// in place (install()), with its own undo history. Heights are of their
// content, in pixels; each sits centred in its margin, headerYPx() /
// footerYPx() below the page's top edge (so a bigger font grows it both
// ways). Saving writes those distances (asDrawn()), so other word
// processors place them the same.

#include "DocumentParts.h"
#include "PageLayout.h"

#include <QByteArray>
#include <QColor>
#include <QObject>

class QPainter;
class QQuickTextDocument;
class QTextDocument;

class HeaderFooter : public QObject
{
    Q_OBJECT

public:
    // Adds the header and footer, empty, to `parts`.
    explicit HeaderFooter(DocumentParts &parts, QObject *parent = nullptr);

    // Their part ids.
    static QString id(bool footer) { return footer ? QStringLiteral("footer") : QStringLiteral("header"); }
    // Whether `part` is the header or footer, and which.
    static bool isPart(const QString &part) { return part == id(false) || part == id(true); }
    static bool isFooter(const QString &part) { return part == id(true); }

    // The header (`footer` false) or footer document.
    QTextDocument *document(bool footer) const { return footer ? m_footer : m_header; }
    bool has(bool footer) const;
    qreal heightPx(bool footer) const;
    qreal headerYPx(const PageLayout &layout) const;
    qreal footerYPx(const PageLayout &layout) const;

    // Installs the header or footer document into a TextEdit's document
    // wrapper, laid out for `layout`.
    void install(bool footer, QQuickTextDocument *qmlDocument, const PageLayout &layout);
    // Inserts a page number (or with `count`, page count) field at
    // `position` in the header or footer, formatted like the text there;
    // returns the position after it.
    int insertPageField(bool footer, int position, bool count);
    // What fields show in the TextEdits: `page`'s numbers, in `color`.
    // The TextEdits draw them blank, keeping their room (from install()):
    // Qt Quick puts a field that starts a centred or right-aligned line at
    // its left edge (QQuickTextNodeEngine::addImage() ignores the alignment
    // there, Qt 6.11). paintFields() draws them over it instead.
    void setEditing(int page, const QColor &color);
    // The page count fields show (and size themselves for).
    void setPageCount(int pages);
    // Both laid out at `layout`'s text width, with tab stops at its centre
    // and right edge.
    void syncWidths(const PageLayout &layout);
    // `layout` with its top and bottom margins grown where the header or
    // footer needs more room: at least 3 mm either side of it.
    PageLayout withRoom(PageLayout layout) const;
    // Draws the header or footer as on `page` (1-based), text in `color`
    // unless formatted otherwise.
    void paint(QPainter *painter, bool footer, int page, const QColor &color);
    // Draws only the header's or footer's fields, where its layout puts
    // them on `page`: over its TextEdit while it is edited.
    void paintFields(QPainter *painter, bool footer, int page, const QColor &color);

    // Empties both, unmodified with no history.
    void clear();

    // `layout` with the header's and footer's distances where they are
    // drawn, as saving writes them (see OdfStyles).
    PageLayout asDrawn(PageLayout layout) const;

signals:
    // Content, height or emptiness changed.
    void changed();

private:
    QTextDocument *m_header;
    QTextDocument *m_footer;
    // The page the TextEdits show fields for, and their color.
    int m_editPage = 1;
    QColor m_editColor = Qt::black;
    int m_pageCount = 1;
};
