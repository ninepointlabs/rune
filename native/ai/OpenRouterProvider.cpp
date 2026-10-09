#include "OpenRouterProvider.h"

#include "AiStorage.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>

namespace {

const QString kKeyKey = QStringLiteral("openrouter/key");
const QString kSignedInKey = QStringLiteral("openrouter/signedIn");

QString errorMessage(const QJsonObject &body)
{
    return body.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString();
}

} // namespace

OpenRouterProvider::OpenRouterProvider(AiStorage *storage, const AiEndpoints &endpoints, BrowserOpener openBrowser,
                                       QObject *parent)
    : AiProvider(storage, endpoints, std::move(openBrowser), parent)
{
    m_signInTimeout.setSingleShot(true);
    m_signInTimeout.setInterval(kSignInTimeoutMs);
    connect(&m_signInTimeout, &QTimer::timeout, this,
            [this] { failSignIn(tr("Sign-in timed out waiting for the browser. Try again.")); });
    connect(&m_loopback, &OAuthLoopback::received, this, &OpenRouterProvider::onRedirect);
    if (m_storage->value(kSignedInKey).toBool())
        setState(State::SignedIn);
}

QUrl OpenRouterProvider::url(const QString &path) const
{
    QUrl u = m_endpoints.openRouter;
    u.setPath(u.path() + path);
    return u;
}

void OpenRouterProvider::signIn()
{
    if (state() == State::SigningIn)
        return;
    if (!m_loopback.listen(QStringLiteral("localhost"), QStringLiteral("/callback"))) {
        setState(State::SignedOut, tr("Couldn't open a local port to receive the sign-in."));
        return;
    }
    m_state = OAuth::randomToken();
    m_verifier = OAuth::randomToken(48);
    QUrl auth = url(QStringLiteral("/auth"));
    const QByteArray query = OAuth::formEncode(
        {{QStringLiteral("callback_url"), m_loopback.redirectUri()},
         {QStringLiteral("code_challenge"), QString::fromLatin1(OAuth::pkceChallenge(m_verifier))},
         {QStringLiteral("code_challenge_method"), QStringLiteral("S256")},
         {QStringLiteral("state"), QString::fromLatin1(m_state)},
         {QStringLiteral("key_label"), QStringLiteral("Rune")}});
    auth.setQuery(QString::fromLatin1(query), QUrl::StrictMode);

    setState(State::SigningIn);
    m_signInTimeout.start();
    if (!m_openBrowser(auth))
        failSignIn(tr("Couldn't open a web browser for the sign-in."));
}

void OpenRouterProvider::cancelSignIn()
{
    if (state() != State::SigningIn)
        return;
    m_loopback.close();
    m_signInTimeout.stop();
    m_state.clear();
    m_verifier.clear();
    setState(State::SignedOut);
}

void OpenRouterProvider::failSignIn(const QString &message)
{
    m_loopback.close();
    m_signInTimeout.stop();
    m_state.clear();
    m_verifier.clear();
    setState(State::SignedOut, message);
}

void OpenRouterProvider::onRedirect(const QUrlQuery &query)
{
    if (state() != State::SigningIn)
        return;
    const QString code = query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
    // OpenRouter sends no state (or code) when the user declines.
    if (code.isEmpty()) {
        failSignIn(tr("Sign-in was cancelled."));
        return;
    }
    if (query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded) != QString::fromLatin1(m_state)) {
        failSignIn(tr("The sign-in response didn't match this sign-in attempt, so it was ignored. Try again."));
        return;
    }
    QNetworkRequest request(url(QStringLiteral("/api/v1/auth/keys")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    const QJsonObject body{{QStringLiteral("code"), code},
                           {QStringLiteral("code_verifier"), QString::fromLatin1(m_verifier)},
                           {QStringLiteral("code_challenge_method"), QStringLiteral("S256")}};
    QNetworkReply *reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (state() != State::SigningIn)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject json = jsonBody(reply->readAll());
        const QString key = json.value(QLatin1String("key")).toString();
        if (status != 200 || key.isEmpty()) {
            failSignIn(status == 0 ? tr("Couldn't reach OpenRouter: %1").arg(reply->errorString())
                       : status == 403 ? tr("OpenRouter didn't accept the sign-in (it may have expired). Try again.")
                                       : tr("OpenRouter didn't complete the sign-in (HTTP %1). Try again.").arg(status));
            return;
        }
        if (!m_storage->setSecret(kKeyKey, key)) {
            failSignIn(tr("Signed in, but the key couldn't be saved to the system keyring."));
            return;
        }
        m_storage->setValue(kSignedInKey, true);
        m_loopback.close();
        m_signInTimeout.stop();
        m_state.clear();
        m_verifier.clear();
        setState(State::SignedIn);
        refreshModels();
    });
}

void OpenRouterProvider::signOut()
{
    cancelSignIn();
    const QString key = m_storage->secret(kKeyKey);
    m_storage->removeSecret(kKeyKey);
    m_storage->removeValue(kSignedInKey);
    setModels({});
    if (key.isEmpty()) {
        setState(State::SignedOut);
        return;
    }
    // OpenRouter's documented per-key page: lowercase hex SHA-256 of the key.
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex());
    setState(State::SignedOut, tr("Signed out. The key Rune used still exists in your OpenRouter account; you can delete it there."),
             QStringLiteral("https://openrouter.ai/keys/") + hash);
}

void OpenRouterProvider::refreshModels()
{
    QNetworkReply *reply = m_network.get(QNetworkRequest(url(QStringLiteral("/api/v1/models"))));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;
        QVariantList models;
        for (const QJsonValue &v : jsonBody(reply->readAll()).value(QLatin1String("data")).toArray()) {
            const QJsonObject m = v.toObject();
            const QString id = m.value(QLatin1String("id")).toString();
            const QStringList families = modelFamilies();
            if (!std::any_of(families.begin(), families.end(), [&](const QString &f) { return id.startsWith(f); }))
                continue;
            // Variants ("...:batch", "...:free") aren't for interactive use.
            if (id.contains(QLatin1Char(':')))
                continue;
            const QString name = m.value(QLatin1String("name")).toString();
            models.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), name.isEmpty() ? id : name}});
        }
        setModels(models);
    });
}

QString OpenRouterProvider::defaultModel() const
{
    for (const QVariant &m : models()) {
        const QString id = m.toMap().value(QStringLiteral("id")).toString();
        if (id.startsWith(QLatin1String("anthropic/")) && id.contains(QLatin1String("sonnet")))
            return id;
    }
    return AiProvider::defaultModel();
}

AiStream *OpenRouterProvider::stream(const AiRequest &request)
{
    auto *stream = new AiStream;
    const QString key = state() == State::SignedIn ? m_storage->secret(kKeyKey) : QString();
    if (key.isEmpty()) {
        if (state() == State::SignedIn) {
            m_storage->removeValue(kSignedInKey);
            setState(State::SignedOut, tr("Rune's OpenRouter key is missing from the system keyring. Sign in again."));
        }
        // Reported once the caller has connected to the stream.
        QMetaObject::invokeMethod(stream, [stream, msg = error().isEmpty() ? tr("Sign in to OpenRouter first.") : error()] {
            stream->fail(QStringLiteral("signed_out"), msg);
        }, Qt::QueuedConnection);
        return stream;
    }
    QJsonArray messages;
    if (!request.instructions.isEmpty())
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                    {QStringLiteral("content"), request.instructions}});
    for (const AiMessage &message : request.messages)
        messages.append(QJsonObject{{QStringLiteral("role"), message.role}, {QStringLiteral("content"), message.text}});
    const QJsonObject body{{QStringLiteral("model"), request.model},
                           {QStringLiteral("messages"), messages},
                           {QStringLiteral("stream"), true}};

    QNetworkRequest http(url(QStringLiteral("/api/v1/chat/completions")));
    http.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    http.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    http.setRawHeader("Accept", "text/event-stream");
    // App attribution, as OpenRouter suggests.
    http.setRawHeader("HTTP-Referer", "https://github.com/ninepointlabs/rune");
    http.setRawHeader("X-Title", "Rune");
    stream->start(
        m_network.post(http, QJsonDocument(body).toJson(QJsonDocument::Compact)),
        [](AiStream *s, const SseParser::Event &event) {
            if (event.data == "[DONE]") {
                s->finish();
                return false;
            }
            const QJsonObject data = jsonBody(event.data);
            if (data.contains(QLatin1String("error"))) {
                const QString message = errorMessage(data);
                s->fail(QStringLiteral("provider_error"),
                        message.isEmpty() ? tr("OpenRouter couldn't complete the response.") : message);
                return false;
            }
            const QJsonObject choice = data.value(QLatin1String("choices")).toArray().first().toObject();
            const QString text = choice.value(QLatin1String("delta")).toObject().value(QLatin1String("content")).toString();
            if (!text.isEmpty())
                s->emitDelta(text);
            if (choice.value(QLatin1String("finish_reason")).toString() == QLatin1String("error")) {
                s->fail(QStringLiteral("provider_error"), tr("The model stopped with an error. Try again."));
                return false;
            }
            return true;
        },
        [this](AiStream *s, int status, const QByteArray &body) { onHttpError(s, status, body); });
    return stream;
}

void OpenRouterProvider::onHttpError(AiStream *stream, int status, const QByteArray &body)
{
    const QString message = errorMessage(jsonBody(body));
    if (status == 401) {
        // The key was deleted or disabled on OpenRouter.
        m_storage->removeSecret(kKeyKey);
        m_storage->removeValue(kSignedInKey);
        setState(State::SignedOut, tr("OpenRouter no longer accepts Rune's key. Sign in again."));
        stream->fail(QStringLiteral("signed_out"), error());
    } else if (status == 402) {
        stream->fail(QStringLiteral("credits"), tr("Your OpenRouter account is out of credits."));
    } else if (status == 429) {
        stream->fail(QStringLiteral("rate_limit"), tr("OpenRouter is rate-limiting requests. Wait a moment and try again."));
    } else {
        stream->fail(QStringLiteral("http_%1").arg(status),
                     message.isEmpty() ? tr("OpenRouter returned an error (HTTP %1).").arg(status) : message);
    }
}
