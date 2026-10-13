#pragma once

// DocumentParts: the document's texts. The body, and every part that is a
// text of its own: the header and footer now, footnotes and endnotes later.
// Each has an id ("" is the body; "header", "footer") and a kind, which says
// what editing it allows. They share the default font, and together make up
// the document's modified state.
//
// A feature owning a kind of part (HeaderFooter for the page margins) adds
// its parts here; editing (DocumentController) routes the selection,
// formatting, undo and typing to whichever part is the edit target, and asks
// the kind whether lists, tables or the AI apply there.

#include <QFont>
#include <QList>
#include <QObject>
#include <QString>

class QTextDocument;

class DocumentParts : public QObject
{
    Q_OBJECT

public:
    enum class Kind {
        Body,
        // A header or footer: one text shown in every page's margin.
        PageMargin,
    };
    // What editing a kind of part allows beyond character and paragraph
    // formatting.
    enum Capability {
        Lists = 0x1,
        Tables = 0x2,
        Ai = 0x4,
        // Typing goes through DocumentController::typeText(), in word-sized
        // undo steps; elsewhere the TextEdit inserts it.
        WordUndoSteps = 0x8,
    };
    Q_DECLARE_FLAGS(Capabilities, Capability)
    static Capabilities capabilities(Kind kind);

    // The body, empty, its unformatted text in `font`.
    explicit DocumentParts(const QFont &font, QObject *parent = nullptr);

    QTextDocument *body() const { return m_parts.constFirst().document; }
    // Adds an empty part (unmodified, no margin of its own) and returns its
    // document; an id already in use returns that part's document.
    QTextDocument *add(const QString &id, Kind kind);
    // The part's document, or nullptr; "" is the body.
    QTextDocument *document(const QString &id) const;
    // The part `doc` is, or "" (the body, or not a part).
    QString idOf(const QTextDocument *doc) const;
    Kind kind(const QString &id) const;
    // Every document, the body first, in the order added.
    QList<QTextDocument *> documents() const;

    bool isModified() const;
    void setModified(bool modified);

signals:
    // Forwarded from each document, so listeners needn't track the parts.
    void modificationChanged();
    void undoRedoAvailabilityChanged(QTextDocument *doc);
    void contentsChange(QTextDocument *doc, int position, int removed, int added);
    void contentsChanged(QTextDocument *doc);

private:
    struct Part
    {
        QString id;
        Kind kind;
        QTextDocument *document;
    };
    QTextDocument *create(Kind kind);

    QFont m_font;
    QList<Part> m_parts;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(DocumentParts::Capabilities)
