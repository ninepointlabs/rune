#include "ChatGptProvider.h"

#include "AiStorage.h"
#include "Jwt.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QUuid>

namespace {

const QString kTokensKey = QStringLiteral("chatgpt/tokens");
const QString kHostIdKey = QStringLiteral("chatgpt/hostId");
const QString kClientIdKey = QStringLiteral("chatgpt/clientId");
const QString kAccountKey = QStringLiteral("chatgpt/account");
const QString kSignedInKey = QStringLiteral("chatgpt/signedIn");

const QString kPlanScope = QStringLiteral("chatgpt.tokens.use.direct");
const QString kScopes = QStringLiteral("openid profile email offline_access resource.invoke ") + kPlanScope;

// Refresh errors after which the refresh token is unusable (Errors and recovery).
const QStringList kTerminalRefreshErrors{
    QStringLiteral("invalid_grant"), QStringLiteral("invalid_refresh_token"), QStringLiteral("token_expired"),
    QStringLiteral("refresh_token_expired"), QStringLiteral("refresh_token_invalidated"),
    QStringLiteral("refresh_token_reused")};

qint64 now()
{
    return QDateTime::currentSecsSinceEpoch();
}

QNetworkRequest formRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/x-www-form-urlencoded"));
    return request;
}

// {"error": "code"} (OAuth) or {"error": {"code": ..., "message": ...}} (API).
QString errorCode(const QJsonObject &body)
{
    const QJsonValue error = body.value(QLatin1String("error"));
    if (error.isString())
        return error.toString();
    const QJsonObject object = error.toObject();
    const QString code = object.value(QLatin1String("code")).toString();
    return code.isEmpty() ? object.value(QLatin1String("type")).toString() : code;
}

QString errorMessage(const QJsonObject &body)
{
    const QJsonValue error = body.value(QLatin1String("error"));
    if (error.isObject())
        return error.toObject().value(QLatin1String("message")).toString();
    return body.value(QLatin1String("error_description")).toString();
}

bool audienceIncludes(const QJsonValue &aud, const QString &clientId)
{
    if (aud.isString())
        return aud.toString() == clientId;
    const QJsonArray list = aud.toArray();
    return std::any_of(list.begin(), list.end(), [&](const QJsonValue &v) { return v.toString() == clientId; });
}

} // namespace

ChatGptProvider::ChatGptProvider(AiStorage *storage, const AiEndpoints &endpoints, BrowserOpener openBrowser,
                                 QObject *parent)
    : AiProvider(storage, endpoints, std::move(openBrowser), parent)
{
    m_signInTimeout.setSingleShot(true);
    m_signInTimeout.setInterval(kSignInTimeoutMs);
    connect(&m_signInTimeout, &QTimer::timeout, this,
            [this] { failSignIn(tr("Sign-in timed out waiting for the browser. Try again.")); });
    connect(&m_loopback, &OAuthLoopback::received, this, &ChatGptProvider::onRedirect);
    // Restored without touching the keyring; tokens are read when needed.
    if (m_storage->value(kSignedInKey).toBool()) {
        setAccount(m_storage->value(kAccountKey).toString());
        setState(State::SignedIn);
    }
}

QUrl ChatGptProvider::authUrl(const QString &path) const
{
    QUrl url = m_endpoints.openAiAuth;
    url.setPath(url.path() + path);
    return url;
}

QUrl ChatGptProvider::apiUrl(const QString &path) const
{
    QUrl url = m_endpoints.openAiApi;
    url.setPath(url.path() + path);
    return url;
}

QString ChatGptProvider::hostId()
{
    QString id = m_storage->value(kHostIdKey).toString();
    if (id.isEmpty()) {
        id = QStringLiteral("urn:uuid:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_storage->setValue(kHostIdKey, id);
    }
    return id;
}

// --- Sign-in -------------------------------------------------------------------

void ChatGptProvider::signIn()
{
    if (state() == State::SigningIn)
        return;
    if (!m_loopback.listen(QStringLiteral("127.0.0.1"), QStringLiteral("/auth/callback"))) {
        setState(State::SignedOut, tr("Couldn't open a local port to receive the sign-in."));
        return;
    }
    m_pending = {OAuth::randomToken(), OAuth::randomToken(), OAuth::randomToken(48), m_loopback.redirectUri(),
                 m_storage->value(kClientIdKey).toString()};

    const bool registering = m_pending.clientId.isEmpty();
    QList<std::pair<QString, QString>> params{
        {QStringLiteral("client_id"), registering ? QStringLiteral("dynamic_agent_client") : m_pending.clientId}};
    if (registering)
        params.append({QStringLiteral("agent_name_hint"), QStringLiteral("Rune")});
    params.append({QStringLiteral("ext_agent_host_id"), hostId()});
    const QString email = m_storage->value(kAccountKey).toString();
    if (!registering && !email.isEmpty())
        params.append({QStringLiteral("login_hint"), email});
    params.append({{QStringLiteral("response_type"), QStringLiteral("code")},
                   {QStringLiteral("redirect_uri"), m_pending.redirectUri},
                   {QStringLiteral("scope"), kScopes},
                   {QStringLiteral("resource"), m_endpoints.openAiApi.toString()},
                   {QStringLiteral("state"), QString::fromLatin1(m_pending.state)},
                   {QStringLiteral("nonce"), QString::fromLatin1(m_pending.nonce)},
                   {QStringLiteral("code_challenge_method"), QStringLiteral("S256")},
                   {QStringLiteral("code_challenge"), QString::fromLatin1(OAuth::pkceChallenge(m_pending.verifier))}});
    QUrl url = authUrl(QStringLiteral("/api/accounts/authorize"));
    url.setQuery(QString::fromLatin1(OAuth::formEncode(params)), QUrl::StrictMode);

    setState(State::SigningIn);
    m_signInTimeout.start();
    if (!m_openBrowser(url))
        failSignIn(tr("Couldn't open a web browser for the sign-in."));
}

void ChatGptProvider::cancelSignIn()
{
    if (state() != State::SigningIn)
        return;
    m_loopback.close();
    m_signInTimeout.stop();
    m_pending = {};
    setState(State::SignedOut);
}

void ChatGptProvider::failSignIn(const QString &message)
{
    m_loopback.close();
    m_signInTimeout.stop();
    m_pending = {};
    setState(State::SignedOut, message);
}

void ChatGptProvider::onRedirect(const QUrlQuery &query)
{
    if (state() != State::SigningIn)
        return;
    const auto item = [&query](const char *name) { return query.queryItemValue(QLatin1String(name), QUrl::FullyDecoded); };
    if (m_pending.state.isEmpty() || item("state") != QString::fromLatin1(m_pending.state)) {
        failSignIn(tr("The sign-in response didn't match this sign-in attempt, so it was ignored. Try again."));
        return;
    }
    if (!item("error").isEmpty()) {
        failSignIn(item("error") == QLatin1String("access_denied")
                       ? tr("Sign-in was cancelled.")
                       : tr("ChatGPT sign-in failed: %1").arg(item("error_description").isEmpty() ? item("error")
                                                                                                  : item("error_description")));
        return;
    }
    // A first registration issues the client_id here; a returning sign-in
    // may omit it, but must not change it.
    QString clientId = item("client_id");
    if (m_pending.clientId.isEmpty()) {
        if (clientId.isEmpty()) {
            failSignIn(tr("ChatGPT didn't register Rune during sign-in. Try again."));
            return;
        }
    } else if (!clientId.isEmpty() && clientId != m_pending.clientId) {
        failSignIn(tr("ChatGPT returned a different app registration than Rune's, so the sign-in was rejected."));
        return;
    } else {
        clientId = m_pending.clientId;
    }
    // Saved now: the registration exists even if the rest fails.
    m_storage->setValue(kClientIdKey, clientId);
    if (item("code").isEmpty()) {
        failSignIn(tr("ChatGPT didn't return a sign-in code. Try again."));
        return;
    }
    exchangeCode(clientId, item("code"));
}

void ChatGptProvider::exchangeCode(const QString &clientId, const QString &code)
{
    const QByteArray body = OAuth::formEncode({{QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
                                        {QStringLiteral("client_id"), clientId},
                                        {QStringLiteral("code"), code},
                                        {QStringLiteral("code_verifier"), QString::fromLatin1(m_pending.verifier)},
                                        {QStringLiteral("redirect_uri"), m_pending.redirectUri},
                                        {QStringLiteral("resource"), m_endpoints.openAiApi.toString()}});
    QNetworkReply *reply = m_network.post(formRequest(authUrl(QStringLiteral("/api/accounts/oauth/token"))), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, clientId] {
        reply->deleteLater();
        if (state() != State::SigningIn)
            return; // cancelled meanwhile
        const QJsonObject json = jsonBody(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200 || json.value(QLatin1String("access_token")).toString().isEmpty()) {
            const QString code = errorCode(json);
            failSignIn(status == 0 ? tr("Couldn't reach ChatGPT: %1").arg(reply->errorString())
                                   : tr("ChatGPT didn't complete the sign-in (%1). Try again.")
                                         .arg(code.isEmpty() ? QStringLiteral("HTTP %1").arg(status) : code));
            return;
        }
        verifyAndStore(clientId, json);
    });
}

void ChatGptProvider::verifyAndStore(const QString &clientId, const QJsonObject &tokens)
{
    QNetworkReply *reply = m_network.get(QNetworkRequest(authUrl(QStringLiteral("/.well-known/jwks.json"))));
    connect(reply, &QNetworkReply::finished, this, [this, reply, clientId, tokens] {
        reply->deleteLater();
        if (state() != State::SigningIn)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            failSignIn(tr("Couldn't fetch ChatGPT's signing keys to verify the sign-in: %1").arg(reply->errorString()));
            return;
        }
        const auto idToken = Jwt::decode(tokens.value(QLatin1String("id_token")).toString().toLatin1());
        QString why;
        if (!idToken) {
            why = QStringLiteral("no valid ID token");
        } else if (Jwt::verifyRs256(*idToken, jsonBody(reply->readAll()), &why)) {
            const QJsonObject &claims = idToken->payload;
            QString issuer = m_endpoints.openAiAuth.toString();
            while (issuer.endsWith(QLatin1Char('/')))
                issuer.chop(1);
            if (claims.value(QLatin1String("iss")).toString() != issuer)
                why = QStringLiteral("wrong issuer");
            else if (!audienceIncludes(claims.value(QLatin1String("aud")), clientId))
                why = QStringLiteral("issued for a different app");
            else if (qint64(claims.value(QLatin1String("exp")).toDouble()) < now() - kClockSkewSecs)
                why = QStringLiteral("expired");
            else if (claims.value(QLatin1String("nonce")).toString() != QString::fromLatin1(m_pending.nonce))
                why = QStringLiteral("nonce mismatch");
            else if (claims.value(QLatin1String("sub")).toString().isEmpty())
                why = QStringLiteral("no account identity");
        }
        if (!why.isEmpty()) {
            failSignIn(tr("ChatGPT's sign-in couldn't be verified (%1), so it wasn't used.").arg(why));
            return;
        }
        const QStringList scopes = tokens.value(QLatin1String("scope")).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (!scopes.contains(kPlanScope)) {
            failSignIn(tr("You signed in, but didn't allow Rune to use your ChatGPT plan, so it can't make "
                          "requests. Sign in again and allow plan usage."));
            return;
        }
        QJsonObject record{
            {QStringLiteral("access_token"), tokens.value(QLatin1String("access_token"))},
            {QStringLiteral("refresh_token"), tokens.value(QLatin1String("refresh_token"))},
            {QStringLiteral("id_token"), tokens.value(QLatin1String("id_token"))},
            {QStringLiteral("scope"), tokens.value(QLatin1String("scope"))},
            {QStringLiteral("expires_at"), double(now() + tokens.value(QLatin1String("expires_in")).toInt(3600))}};
        if (!m_storage->setSecret(kTokensKey, QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact)))) {
            failSignIn(tr("Signed in, but the sign-in couldn't be saved to the system keyring."));
            return;
        }
        const QString email = idToken->payload.value(QLatin1String("email")).toString();
        m_storage->setValue(kAccountKey, email);
        m_storage->setValue(kSignedInKey, true);
        m_loopback.close();
        m_signInTimeout.stop();
        m_pending = {};
        setAccount(email);
        setState(State::SignedIn);
        refreshModels();
    });
}

// --- Tokens --------------------------------------------------------------------

QJsonObject ChatGptProvider::loadTokens()
{
    return jsonBody(m_storage->secret(kTokensKey).toUtf8());
}

void ChatGptProvider::saveTokens(const QJsonObject &tokens)
{
    m_storage->setSecret(kTokensKey, QString::fromUtf8(QJsonDocument(tokens).toJson(QJsonDocument::Compact)));
}

void ChatGptProvider::forgetTokens()
{
    m_storage->removeSecret(kTokensKey);
    m_storage->removeValue(kSignedInKey);
}

void ChatGptProvider::withAccessToken(std::function<void(const QString &, const QString &)> ready)
{
    if (state() != State::SignedIn) {
        ready(QString(), tr("Sign in to ChatGPT first."));
        return;
    }
    const QJsonObject tokens = loadTokens();
    const QString access = tokens.value(QLatin1String("access_token")).toString();
    if (access.isEmpty()) {
        forgetTokens();
        setState(State::SignedOut, tr("Rune's ChatGPT sign-in is missing from the system keyring. Sign in again."));
        ready(QString(), error());
        return;
    }
    if (qint64(tokens.value(QLatin1String("expires_at")).toDouble()) - now() > kRefreshMarginSecs) {
        ready(access, QString());
        return;
    }
    m_waiting.append(std::move(ready));
    if (!m_refreshing)
        refresh();
}

void ChatGptProvider::refresh()
{
    m_refreshing = true;
    const QJsonObject tokens = loadTokens();
    const QString clientId = m_storage->value(kClientIdKey).toString();
    const QByteArray body = OAuth::formEncode({{QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
                                        {QStringLiteral("client_id"), clientId},
                                        {QStringLiteral("refresh_token"), tokens.value(QLatin1String("refresh_token")).toString()},
                                        {QStringLiteral("resource"), m_endpoints.openAiApi.toString()}});
    QNetworkReply *reply = m_network.post(formRequest(authUrl(QStringLiteral("/api/accounts/oauth/token"))), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, tokens] {
        reply->deleteLater();
        const QJsonObject json = jsonBody(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 200 && !json.value(QLatin1String("access_token")).toString().isEmpty()) {
            // Access token, expiry, scope and the rotated refresh token together.
            QJsonObject updated = tokens;
            updated.insert(QStringLiteral("access_token"), json.value(QLatin1String("access_token")));
            if (json.contains(QLatin1String("refresh_token")))
                updated.insert(QStringLiteral("refresh_token"), json.value(QLatin1String("refresh_token")));
            if (json.contains(QLatin1String("scope")))
                updated.insert(QStringLiteral("scope"), json.value(QLatin1String("scope")));
            updated.insert(QStringLiteral("expires_at"),
                           double(now() + json.value(QLatin1String("expires_in")).toInt(3600)));
            saveTokens(updated);
            finishRefresh(updated.value(QLatin1String("access_token")).toString(), QString());
            return;
        }
        const QString code = errorCode(json);
        if (kTerminalRefreshErrors.contains(code)) {
            forgetTokens();
            setState(State::SignedOut, tr("Your ChatGPT sign-in has expired. Sign in again."));
            finishRefresh(QString(), error());
        } else if (code == QLatin1String("invalid_client")) {
            // Rune's registration is gone: register afresh next time.
            forgetTokens();
            m_storage->removeValue(kClientIdKey);
            setState(State::SignedOut, tr("ChatGPT no longer recognizes Rune's registration. Sign in again."));
            finishRefresh(QString(), error());
        } else {
            // Network or server trouble: keep the sign-in, try again later.
            finishRefresh(QString(), status == 0 ? tr("Couldn't reach ChatGPT to renew the sign-in: %1").arg(reply->errorString())
                                                 : tr("ChatGPT couldn't renew the sign-in right now (%1). Try again shortly.")
                                                       .arg(code.isEmpty() ? QStringLiteral("HTTP %1").arg(status) : code));
        }
    });
}

void ChatGptProvider::finishRefresh(const QString &token, const QString &error)
{
    m_refreshing = false;
    const auto waiting = std::exchange(m_waiting, {});
    for (const auto &ready : waiting)
        ready(token, error);
}

// --- Sign-out --------------------------------------------------------------------

void ChatGptProvider::signOut()
{
    cancelSignIn();
    const QString refreshToken = loadTokens().value(QLatin1String("refresh_token")).toString();
    const QString clientId = m_storage->value(kClientIdKey).toString();
    // Forgotten locally at once; the client_id and host id are kept.
    forgetTokens();
    setModels({});
    setState(State::SignedOut);
    if (refreshToken.isEmpty() || clientId.isEmpty())
        return;
    const QByteArray body = OAuth::formEncode({{QStringLiteral("token"), refreshToken},
                                        {QStringLiteral("token_type_hint"), QStringLiteral("refresh_token")},
                                        {QStringLiteral("client_id"), clientId}});
    QNetworkReply *reply = m_network.post(formRequest(authUrl(QStringLiteral("/api/accounts/oauth/revoke"))), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200 && state() == State::SignedOut)
            setState(State::SignedOut, tr("Signed out on this computer, but ChatGPT didn't confirm it (%1).")
                                           .arg(status == 0 ? reply->errorString() : QStringLiteral("HTTP %1").arg(status)));
    });
}

// --- Models and requests -------------------------------------------------------------

void ChatGptProvider::refreshModels()
{
    withAccessToken([this](const QString &token, const QString &) {
        if (token.isEmpty())
            return;
        QNetworkRequest request(apiUrl(QStringLiteral("/models")));
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        QNetworkReply *reply = m_network.get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            const QJsonObject json = jsonBody(reply->readAll());
            QVariantList models;
            // The documented shape: {"models": [{slug, display_name, visibility}]}.
            for (const QJsonValue &v : json.value(QLatin1String("models")).toArray()) {
                const QJsonObject m = v.toObject();
                if (m.value(QLatin1String("visibility")).toString() != QLatin1String("list"))
                    continue;
                const QString slug = m.value(QLatin1String("slug")).toString();
                const QString display = m.value(QLatin1String("display_name")).toString();
                models.append(QVariantMap{{QStringLiteral("id"), slug},
                                          {QStringLiteral("name"), display.isEmpty() ? slug : display}});
            }
            if (!models.isEmpty() || reply->error() == QNetworkReply::NoError)
                setModels(models);
        });
    });
}

AiStream *ChatGptProvider::stream(const AiRequest &request)
{
    auto *stream = new AiStream;
    QJsonArray input;
    for (const AiMessage &message : request.messages)
        input.append(QJsonObject{{QStringLiteral("role"), message.role}, {QStringLiteral("content"), message.text}});
    QJsonObject body{{QStringLiteral("model"), request.model},
                     {QStringLiteral("input"), input},
                     {QStringLiteral("store"), false},
                     {QStringLiteral("stream"), true}};
    if (!request.instructions.isEmpty())
        body.insert(QStringLiteral("instructions"), request.instructions);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QPointer<AiStream> guard(stream);
    withAccessToken([this, guard, payload](const QString &token, const QString &error) {
        if (!guard || guard->isDone())
            return;
        if (token.isEmpty()) {
            guard->fail(state() == State::SignedIn ? QStringLiteral("network") : QStringLiteral("signed_out"), error);
            return;
        }
        QNetworkRequest http(apiUrl(QStringLiteral("/responses")));
        http.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        http.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
        http.setRawHeader("Accept", "text/event-stream");
        guard->start(
            m_network.post(http, payload),
            [](AiStream *s, const SseParser::Event &event) {
                const QJsonObject data = jsonBody(event.data);
                const QString type = data.value(QLatin1String("type")).toString(event.event);
                if (type == QLatin1String("response.output_text.delta")) {
                    s->emitDelta(data.value(QLatin1String("delta")).toString());
                } else if (type == QLatin1String("response.completed")) {
                    s->finish();
                    return false;
                } else if (type == QLatin1String("response.failed")) {
                    const QJsonObject err = data.value(QLatin1String("response")).toObject().value(QLatin1String("error")).toObject();
                    failFromCode(s, err.value(QLatin1String("code")).toString(QStringLiteral("unknown_error")),
                                 err.value(QLatin1String("message")).toString());
                    return false;
                } else if (type == QLatin1String("response.incomplete")) {
                    const QString reason = data.value(QLatin1String("response")).toObject()
                                               .value(QLatin1String("incomplete_details")).toObject()
                                               .value(QLatin1String("reason")).toString();
                    s->fail(QStringLiteral("incomplete"), tr("The response stopped early (%1).").arg(reason));
                    return false;
                } else if (type == QLatin1String("error")) {
                    failFromCode(s, data.value(QLatin1String("code")).toString(errorCode(data)),
                                 data.value(QLatin1String("message")).toString(errorMessage(data)));
                    return false;
                }
                return true;
            },
            [this](AiStream *s, int status, const QByteArray &body) { onHttpError(s, status, body); });
    });
    return stream;
}

void ChatGptProvider::onHttpError(AiStream *stream, int status, const QByteArray &body)
{
    const QJsonObject json = jsonBody(body);
    const QString code = errorCode(json);
    const QString message = errorMessage(json);
    if (!code.isEmpty() && (code.startsWith(QLatin1String("subscription_sharing_")) || code.startsWith(QLatin1String("chatpass_")))) {
        failFromCode(stream, code, message);
    } else if (status == 401) {
        stream->fail(QStringLiteral("unauthorized"),
                     tr("ChatGPT didn't accept this sign-in. If it keeps happening, sign out and sign in again."));
    } else if (status == 403) {
        stream->fail(QStringLiteral("forbidden"), tr("ChatGPT blocked this request: %1")
                                                      .arg(message.isEmpty() ? QStringLiteral("HTTP 403") : message));
    } else if (status == 503) {
        stream->fail(QStringLiteral("unavailable"), tr("ChatGPT is temporarily unavailable. Try again shortly."));
    } else {
        stream->fail(code.isEmpty() ? QStringLiteral("http_%1").arg(status) : code,
                     message.isEmpty() ? tr("ChatGPT returned an error (HTTP %1).").arg(status) : message);
    }
}

void ChatGptProvider::failFromCode(AiStream *stream, const QString &code, const QString &fallback)
{
    if (code == QLatin1String("subscription_sharing_usage_limit_exceeded"))
        stream->fail(QStringLiteral("usage_limit"),
                     tr("Usage limit reached. Review your plan or this app's limit in ChatGPT settings."));
    else if (code == QLatin1String("subscription_sharing_user_not_eligible"))
        stream->fail(QStringLiteral("not_eligible"),
                     tr("Your ChatGPT plan or workspace can't be used in other apps, so Rune can't use it."));
    else if (code == QLatin1String("subscription_sharing_usage_unavailable")
             || code == QLatin1String("subscription_sharing_user_unavailable"))
        stream->fail(QStringLiteral("unavailable"), tr("ChatGPT usage is temporarily unavailable. Try again shortly."));
    else if (code == QLatin1String("subscription_sharing_invalid_user"))
        stream->fail(QStringLiteral("unauthorized"),
                     tr("ChatGPT didn't accept this sign-in. If it keeps happening, sign out and sign in again."));
    else if (code.startsWith(QLatin1String("chatpass_")))
        stream->fail(QStringLiteral("config"), tr("ChatGPT rejected Rune's request permissions (%1).").arg(code));
    else
        stream->fail(code, fallback.isEmpty() ? tr("ChatGPT couldn't complete the response (%1).").arg(code) : fallback);
}
