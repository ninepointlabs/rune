#pragma once

// ClipboardHelper: the system clipboard (plain text) for QML.
//
//   ClipboardHelper { id: clipboard }
//   clipboard.setText("copied"); const t = clipboard.text()

#include <QObject>
#include <QQmlEngine>
#include <QString>

class ClipboardHelper : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit ClipboardHelper(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE QString text() const;
    Q_INVOKABLE void setText(const QString &text);
};
