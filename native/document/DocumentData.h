#pragma once

// DocumentData: what a document carries besides its text. Named stores of
// JSON (an object or array each): the story notes and the sources list,
// later. They are part of the document: saved inside its file (an .odt's
// package, a .docx's custom XML part; see OdfWriter, OdfReader, DocxData),
// counted in its modified state, and emptied by a new document. Both people
// (through the UI) and agents (through the same JSON) edit them.
//
// Not in the undo history: changing a store is not a text edit.

#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

class DocumentData : public QObject
{
    Q_OBJECT

public:
    // Every store, by name.
    using Stores = QMap<QString, QJsonValue>;

    using QObject::QObject;

    // Whether `name` can name a store: 1 to 64 of a-z, 0-9 and "-" (it
    // becomes a file name inside the document).
    static bool isValidName(const QString &name);

    // The store, or an undefined value if there is none.
    QJsonValue value(const QString &name) const { return m_stores.value(name, QJsonValue(QJsonValue::Undefined)); }
    // Sets the store (an object or array) and marks the data modified; an
    // undefined or null `value` removes it. False, changing nothing, for an
    // invalid name or another kind of value.
    bool setValue(const QString &name, const QJsonValue &value);
    QStringList names() const { return m_stores.keys(); }
    const Stores &stores() const { return m_stores; }
    // Replaces every store with `stores` (as read from a file): unmodified.
    // Invalid names and values are left out.
    void replace(const Stores &stores);

    bool isModified() const { return m_modified; }
    void setModified(bool modified);

signals:
    void changed(const QString &name);
    void modificationChanged();

private:
    Stores m_stores;
    bool m_modified = false;
};
