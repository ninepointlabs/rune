#include "DocumentParts.h"

#include "InlineObjects.h"

#include <QTextDocument>

DocumentParts::Capabilities DocumentParts::capabilities(Kind kind)
{
    switch (kind) {
    case Kind::Body: return Lists | Tables | Ai | WordUndoSteps;
    case Kind::PageMargin: return {};
    }
    return {};
}

DocumentParts::DocumentParts(const QFont &font, QObject *parent) : QObject(parent), m_font(font)
{
    m_parts.append({QString(), Kind::Body, create(Kind::Body)});
}

QTextDocument *DocumentParts::create(Kind kind)
{
    auto *doc = new QTextDocument(this);
    doc->setDefaultFont(m_font);
    // A part is laid out in an area of the page; the body's margins are
    // the page margins (PageLayout::storeIn()).
    if (kind != Kind::Body)
        doc->setDocumentMargin(0);
    InlineObjects::installAll(doc, kind);
    // Setting the margin is a recorded format change: a new part starts
    // unmodified, with no history.
    doc->clearUndoRedoStacks();
    doc->setModified(false);
    connect(doc, &QTextDocument::modificationChanged, this, &DocumentParts::modificationChanged);
    connect(doc, &QTextDocument::undoAvailable, this, [this, doc] { emit undoRedoAvailabilityChanged(doc); });
    connect(doc, &QTextDocument::redoAvailable, this, [this, doc] { emit undoRedoAvailabilityChanged(doc); });
    connect(doc, &QTextDocument::contentsChange, this,
            [this, doc](int position, int removed, int added) { emit contentsChange(doc, position, removed, added); });
    connect(doc, &QTextDocument::contentsChanged, this, [this, doc] { emit contentsChanged(doc); });
    return doc;
}

QTextDocument *DocumentParts::add(const QString &id, Kind kind)
{
    if (QTextDocument *existing = document(id))
        return existing;
    QTextDocument *doc = create(kind);
    m_parts.append({id, kind, doc});
    return doc;
}

QTextDocument *DocumentParts::document(const QString &id) const
{
    for (const Part &part : m_parts)
        if (part.id == id)
            return part.document;
    return nullptr;
}

QString DocumentParts::idOf(const QTextDocument *doc) const
{
    for (const Part &part : m_parts)
        if (part.document == doc)
            return part.id;
    return {};
}

DocumentParts::Kind DocumentParts::kind(const QString &id) const
{
    for (const Part &part : m_parts)
        if (part.id == id)
            return part.kind;
    return Kind::Body;
}

QList<QTextDocument *> DocumentParts::documents() const
{
    QList<QTextDocument *> docs;
    for (const Part &part : m_parts)
        docs.append(part.document);
    return docs;
}

bool DocumentParts::isModified() const
{
    for (const Part &part : m_parts)
        if (part.document->isModified())
            return true;
    return false;
}

void DocumentParts::setModified(bool modified)
{
    for (const Part &part : m_parts)
        part.document->setModified(modified);
}
