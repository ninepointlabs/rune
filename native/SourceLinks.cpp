#include "SourceLinks.h"

#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

SourceLinks::SourceLinks(QObject *parent) : QObject(parent) {}

QUrl SourceLinks::linkFor(const QJsonObject &item) const
{
    QString doi = item.value(QStringLiteral("DOI")).toString().trimmed();
    doi.remove(QRegularExpression(QStringLiteral("^(https?://(dx\\.)?doi\\.org/|doi:)"), QRegularExpression::CaseInsensitiveOption));
    if (!doi.isEmpty())
        return QUrl(m_doiResolver + doi);
    const QUrl url(item.value(QStringLiteral("URL")).toString().trimmed());
    return url.isValid() && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) ? url : QUrl();
}

void SourceLinks::check(const QString &id, const QUrl &url)
{
    if (QNetworkReply *previous = m_running.take(id)) {
        previous->disconnect(this);
        previous->abort();
        previous->deleteLater();
    }
    if (!url.isValid()) {
        emit checked(id, false);
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(m_timeoutMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Rune (source link check)"));
    QNetworkReply *reply = m_network.get(request);
    m_running.insert(id, reply);
    // The status is all it takes: stop reading once the answer starts.
    auto status = std::make_shared<int>(0);
    connect(reply, &QNetworkReply::readyRead, reply, [reply, status] {
        *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, status] {
        if (m_running.value(id) == reply)
            m_running.remove(id);
        reply->deleteLater();
        int code = *status;
        if (code == 0)
            code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        emit checked(id, code >= 200 && code < 400);
    });
}
