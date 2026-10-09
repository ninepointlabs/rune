#pragma once

// OdfExporter: writes a QML TextEdit's QTextDocument to ODF (.odt).
//
//   OdfExporter { id: exporter }
//   exporter.exportToOdf(editor.textDocument, "/tmp/out.odt")
//   console.log(exporter.describe(editor.textDocument))

#include <QObject>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QString>

class OdfExporter : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit OdfExporter(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE bool exportToOdf(QQuickTextDocument *doc, const QString &path);

    // Structural dump of the QTextDocument (frames, tables, lists, char
    // formats) so the spike can verify what Qt actually built from the HTML.
    Q_INVOKABLE QString describe(QQuickTextDocument *doc) const;
};
