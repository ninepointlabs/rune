#pragma once

// Citations: citations in the body and the bibliography they make.
//
// A citation is an inline object (InlineObjects) whose format holds the ids
// of the sources it cites (Sources, CSL-JSON in the "sources" store) and a
// locator (page). It is drawn as its in-text citation in the document's
// style ("(Smith, 2020, p. 12)", "[3]"), so changing the style or a source
// redraws every citation. The AI sees it as Pandoc's [@smith2020, p. 12].
//
// The bibliography is ordinary paragraphs, marked as such (FormatIds::
// kBibliography): a heading, then one entry per cited source. Rune rewrites
// it whenever what is cited, the sources or the style change, so edits to
// it don't last.
//
// Saved as the citation's text between two bookmarks named _RuneCite<n>
// (Word and LibreOffice keep bookmarks, and show the text), with what each
// <n> cites in the "citations" data store. Read back, a citation whose
// bookmark the store doesn't explain keeps just its text.

#include "CitationStyles.h"
#include "InlineObjects.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QColor>
#include <QObject>
#include <QStringList>
#include <QTextObjectInterface>

class QTextCursor;
class QTextDocument;

namespace Citations {

// The citation style: {"style": "apa"}.
inline const QString kSettingsStore = QStringLiteral("citation-settings");
// What each saved citation's bookmark cites: {"1": {"ids": [...], "locator": "..."}}.
inline const QString kSavedStore = QStringLiteral("citations");
inline const QString kBookmarkPrefix = QStringLiteral("_RuneCite");

QTextCharFormat format(const QStringList &ids, const QString &locator, const QTextCharFormat &base);
bool isCitation(const QTextFormat &format);
QStringList ids(const QTextFormat &format);
QString locator(const QTextFormat &format);

// "[@a; @b, p. 12]", and back: each [@...] in `text` that cites only keys,
// with its position.
QString toMarkdown(const QStringList &ids, const QString &locator);
struct MarkdownCitation
{
    qsizetype start = 0;
    qsizetype length = 0;
    QStringList ids;
    QString locator;
};
QList<MarkdownCitation> findInMarkdown(const QString &text);

InlineObjects::Kind kind();

// What renders a document's citations: its sources and style, and the
// order sources are first cited in (IEEE's numbers, its bibliography).
// One per body document, its child.
class Engine : public QObject
{
    Q_OBJECT

public:
    static Engine *of(const QTextDocument *doc);
    // The document's engine, made if it has none.
    static Engine *attach(QTextDocument *doc);

    const QJsonArray &sources() const { return m_sources; }
    CitationStyles::Style style() const { return m_style; }
    // Each redraws the citations if anything changed.
    void setSources(const QJsonArray &sources);
    void setStyle(CitationStyles::Style style);
    // Numbers the sources by where they are first cited; redraws.
    void refresh();

    // The in-text citation for a citation's format.
    QString render(const QTextFormat &format) const;
    // What citations are drawn in unless formatted otherwise: the text's
    // color (the view's, which the document doesn't know).
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    // The ids in the order first cited.
    const QStringList &citedOrder() const { return m_citedOrder; }
    // The cited sources in bibliography order; ids no source has left out.
    QList<QJsonObject> bibliographySources() const;

private:
    explicit Engine(QTextDocument *doc);
    void redraw();

    QTextDocument *m_doc;
    QJsonArray m_sources;
    CitationStyles::Style m_style = CitationStyles::Style::Apa;
    QStringList m_citedOrder;
    QColor m_color = Qt::black;
};

// Draws citations as their Engine renders them.
class Handler : public QObject, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    using QObject::QObject;
    static void install(QTextDocument *doc);

    QSizeF intrinsicSize(QTextDocument *doc, int position, const QTextFormat &format) override;
    void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *doc, int position,
                    const QTextFormat &format) override;
};

bool hasBibliography(const QTextDocument *doc);
// Writes the bibliography: over the existing one, or at `cursor` (as
// paragraphs of its own) if there is none and `insert`. An undo step of its
// own, or with `joinPrevious` part of the last one (an update following
// the edit that caused it). False, changing nothing, if there is none to
// update or it is already up to date.
bool writeBibliography(QTextCursor cursor, bool insert, bool joinPrevious);

// After reading a file: citations read back from bookmarks get what they
// cite from `saved` (the "citations" store); any it doesn't explain become
// their text again. Not an undoable edit on its own: the caller clears the
// history.
void resolveSaved(QTextDocument *doc, const QJsonObject &saved);

} // namespace Citations
