#include "AiProvider.h"

#include <QJsonDocument>
#include <QNetworkReply>

// --- AiStream ----------------------------------------------------------------

AiStream::~AiStream()
{
    if (m_reply)
        m_reply->abort();
}

void AiStream::start(QNetworkReply *reply, EventHandler onEvent, HttpErrorHandler onHttpError)
{
    m_reply = reply;
    reply->setParent(this);
    m_onEvent = std::move(onEvent);
    m_onHttpError = std::move(onHttpError);
    if (m_done) { // cancelled or failed before the request went out
        reply->abort();
        return;
    }
    connect(reply, &QNetworkReply::readyRead, this, &AiStream::onReadyRead);
    connect(reply, &QNetworkReply::finished, this, &AiStream::onReplyFinished);
}

void AiStream::cancel()
{
    m_done = true;
    if (m_reply)
        m_reply->abort();
}

void AiStream::finish()
{
    if (m_done)
        return;
    m_done = true;
    emit finished();
}

void AiStream::fail(const QString &code, const QString &message)
{
    if (m_done)
        return;
    m_done = true;
    if (m_reply)
        m_reply->abort();
    emit failed(code, message);
}

void AiStream::onReadyRead()
{
    if (!m_reply)
        return;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) { // an error body, not a stream: handled once complete
        m_errorBody += m_reply->readAll();
        return;
    }
    const QByteArray bytes = m_reply->readAll();
    for (const SseParser::Event &event : m_parser.feed(bytes)) {
        if (m_done || !m_onEvent(this, event))
            return;
    }
}

void AiStream::onReplyFinished()
{
    if (m_done || !m_reply)
        return;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) {
        m_errorBody += m_reply->readAll();
        m_onHttpError(this, status, m_errorBody);
        fail(QStringLiteral("http_%1").arg(status), tr("The request failed (HTTP %1).").arg(status)); // if the handler didn't
        return;
    }
    if (m_reply->error() != QNetworkReply::NoError) {
        fail(QStringLiteral("network"), tr("Network error: %1").arg(m_reply->errorString()));
        return;
    }
    onReadyRead(); // anything left
    fail(QStringLiteral("interrupted"), tr("The response was cut off before it finished. Try again."));
}

// --- AiProvider --------------------------------------------------------------

AiProvider::AiProvider(AiStorage *storage, const AiEndpoints &endpoints, BrowserOpener openBrowser, QObject *parent)
    : QObject(parent), m_storage(storage), m_endpoints(endpoints), m_openBrowser(std::move(openBrowser))
{
}

void AiProvider::cancelSignIn()
{
    if (m_state == State::SigningIn)
        setState(State::SignedOut);
}

QString AiProvider::defaultModel() const
{
    return m_models.isEmpty() ? QString() : m_models.constFirst().toMap().value(QStringLiteral("id")).toString();
}

void AiProvider::setState(State state, const QString &error, const QString &errorLink)
{
    if (m_state == state && m_error == error && m_errorLink == errorLink)
        return;
    m_state = state;
    m_error = error;
    m_errorLink = errorLink;
    emit stateChanged();
}

void AiProvider::setAccount(const QString &account)
{
    if (m_account == account)
        return;
    m_account = account;
    emit stateChanged();
}

void AiProvider::setModels(const QVariantList &models)
{
    m_models = models;
    emit modelsChanged();
}

QJsonObject AiProvider::jsonBody(const QByteArray &body)
{
    return QJsonDocument::fromJson(body).object();
}
