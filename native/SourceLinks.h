#pragma once

// SourceLinks: whether a source's DOI or URL resolves. Models invent
// plausible-looking references, so a source an agent adds is checked: its
// DOI through the DOI resolver (https://doi.org/), else its URL. A link
// that answers, after redirects, with a status below 400 is "ok"; an error
// status or no answer within the timeout is "broken". Only the start of the
// answer is read.

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

class QJsonObject;
class QNetworkReply;

class SourceLinks : public QObject
{
    Q_OBJECT

public:
    explicit SourceLinks(QObject *parent = nullptr);

    // Where DOIs are resolved ("https://doi.org/"; tests point it at a
    // local server).
    void setDoiResolver(const QString &base) { m_doiResolver = base; }
    void setTimeout(int ms) { m_timeoutMs = ms; }

    // The link a source would be checked at, or an empty URL if it has
    // neither a DOI nor an http(s) URL.
    QUrl linkFor(const QJsonObject &item) const;
    // Checks source `id`'s link; checked() follows. A check of the same
    // source still running is replaced.
    void check(const QString &id, const QUrl &url);

signals:
    void checked(const QString &id, bool ok);

private:
    QNetworkAccessManager m_network;
    QHash<QString, QNetworkReply *> m_running;
    QString m_doiResolver = QStringLiteral("https://doi.org/");
    int m_timeoutMs = 15000;
};
