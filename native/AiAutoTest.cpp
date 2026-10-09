#include "AiAutoTest.h"

#include "AiManager.h"
#include "AiStorage.h"
#include "DocumentController.h"
#include "OdfReader.h"
#include "ChatGptProvider.h"
#include "Jwt.h"
#include "OAuthLoopback.h"
#include "OpenRouterProvider.h"
#include "SseParser.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>
#include <QTextTable>
#include <QUrlQuery>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <memory>

namespace {

using Check = std::function<void(bool, const QString &)>;

bool waitFor(const std::function<bool()> &done, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 20);
    return done();
}

// --- A minimal HTTP/1.1 server for the providers to talk to -----------------

struct HttpRequest
{
    QByteArray method;
    QString path;
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers; // lower-case names
    QByteArray body;
    QUrlQuery form() const { return QUrlQuery(QString::fromUtf8(body)); }
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

struct HttpResponse
{
    int status = 200;
    QByteArray contentType = "application/json";
    QByteArray body;
    // Send the body as one chunk of a chunked response and keep the
    // connection open: a stream still in progress.
    bool holdOpen = false;
};

HttpResponse json(const QJsonObject &object, int status = 200)
{
    return {status, "application/json", QJsonDocument(object).toJson(QJsonDocument::Compact)};
}

HttpResponse sse(const QByteArray &body)
{
    return {200, "text/event-stream", body};
}

class MockServer : public QObject
{
public:
    using Handler = std::function<HttpResponse(const HttpRequest &)>;

    bool start()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onData(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    // Handlers by "METHOD /path"; unknown routes get a 404.
    QHash<QString, Handler> routes;
    QList<HttpRequest> requests;

    int count(const QString &route) const
    {
        return int(std::count_if(requests.begin(), requests.end(), [&](const HttpRequest &r) {
            return QString::fromLatin1(r.method) + ' ' + r.path == route;
        }));
    }
    const HttpRequest *last(const QString &route) const
    {
        for (auto it = requests.rbegin(); it != requests.rend(); ++it)
            if (QString::fromLatin1(it->method) + ' ' + it->path == route)
                return &*it;
        return nullptr;
    }

private:
    void onData(QTcpSocket *socket)
    {
        QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            socket->setProperty("buffer", buffer);
            return;
        }
        HttpRequest request;
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        request.method = requestLine.value(0);
        const QUrl target(QString::fromLatin1(requestLine.value(1)));
        request.path = target.path();
        request.query = QUrlQuery(target);
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines[i].trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon > 0)
                request.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
        }
        const int length = request.headers.value("content-length").toInt();
        if (buffer.size() - headerEnd - 4 < length) {
            socket->setProperty("buffer", buffer);
            return;
        }
        socket->setProperty("buffer", QByteArray());
        request.body = buffer.mid(headerEnd + 4, length);
        requests.append(request);
        const Handler handler = routes.value(QString::fromLatin1(request.method) + ' ' + request.path);
        const HttpResponse response = handler ? handler(request) : HttpResponse{404, "application/json", "{}"};
        if (response.holdOpen) {
            socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " X\r\nContent-Type: " + response.contentType
                          + "\r\nTransfer-Encoding: chunked\r\n\r\n" + QByteArray::number(response.body.size(), 16)
                          + "\r\n" + response.body + "\r\n");
            return; // no final chunk: the client sees a stream still going
        }
        socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " X\r\nContent-Type: " + response.contentType
                      + "\r\nContent-Length: " + QByteArray::number(response.body.size())
                      + "\r\nConnection: close\r\n\r\n" + response.body);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
};

// --- RS256 signing for test ID tokens ----------------------------------------

struct TestKey
{
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{EVP_RSA_gen(2048), &EVP_PKEY_free};
    QString kid = QStringLiteral("test-key");

    QByteArray bn(const char *name) const
    {
        BIGNUM *value = nullptr;
        EVP_PKEY_get_bn_param(key.get(), name, &value);
        QByteArray bytes(BN_num_bytes(value), Qt::Uninitialized);
        BN_bn2bin(value, reinterpret_cast<unsigned char *>(bytes.data()));
        BN_free(value);
        return bytes;
    }
    QJsonObject jwks() const
    {
        return {{QStringLiteral("keys"),
                 QJsonArray{QJsonObject{{QStringLiteral("kty"), QStringLiteral("RSA")},
                                        {QStringLiteral("kid"), kid},
                                        {QStringLiteral("alg"), QStringLiteral("RS256")},
                                        {QStringLiteral("use"), QStringLiteral("sig")},
                                        {QStringLiteral("n"), QString::fromLatin1(OAuth::base64Url(bn(OSSL_PKEY_PARAM_RSA_N)))},
                                        {QStringLiteral("e"), QString::fromLatin1(OAuth::base64Url(bn(OSSL_PKEY_PARAM_RSA_E)))}}}}};
    }
    QByteArray sign(const QJsonObject &claims, const QString &useKid = QString()) const
    {
        const QJsonObject header{{QStringLiteral("alg"), QStringLiteral("RS256")},
                                 {QStringLiteral("typ"), QStringLiteral("JWT")},
                                 {QStringLiteral("kid"), useKid.isEmpty() ? kid : useKid}};
        const QByteArray input = OAuth::base64Url(QJsonDocument(header).toJson(QJsonDocument::Compact)) + '.'
            + OAuth::base64Url(QJsonDocument(claims).toJson(QJsonDocument::Compact));
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
        size_t length = 0;
        EVP_DigestSignInit(md.get(), nullptr, EVP_sha256(), nullptr, key.get());
        EVP_DigestSign(md.get(), nullptr, &length, reinterpret_cast<const unsigned char *>(input.constData()), input.size());
        QByteArray signature(int(length), Qt::Uninitialized);
        EVP_DigestSign(md.get(), reinterpret_cast<unsigned char *>(signature.data()), &length,
                       reinterpret_cast<const unsigned char *>(input.constData()), input.size());
        signature.resize(int(length));
        return input + '.' + OAuth::base64Url(signature);
    }
};

// What the "browser" was asked to open, and following its redirect.
struct Browser
{
    QList<QUrl> opened;
    QNetworkAccessManager network;

    // GETs `redirect` + `params`, as the provider's page would redirect.
    int follow(const QString &redirect, const QList<std::pair<QString, QString>> &params)
    {
        QUrl url(redirect);
        url.setQuery(QString::fromLatin1(OAuth::formEncode(params)), QUrl::StrictMode);
        QNetworkReply *reply = network.get(QNetworkRequest(url));
        waitFor([reply] { return reply->isFinished(); });
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        return status;
    }
};

QString param(const QUrl &url, const char *name)
{
    return QUrlQuery(url).queryItemValue(QLatin1String(name), QUrl::FullyDecoded);
}

// An OpenRouter-style stream delivering `pieces`; `complete` adds [DONE].
QByteArray chatStream(const QStringList &pieces, bool complete = true)
{
    QByteArray out;
    for (const QString &piece : pieces) {
        const QJsonObject chunk{{QStringLiteral("choices"),
                                 QJsonArray{QJsonObject{{QStringLiteral("delta"), QJsonObject{{QStringLiteral("content"), piece}}}}}}};
        out += "data: " + QJsonDocument(chunk).toJson(QJsonDocument::Compact) + "\n\n";
    }
    if (complete)
        out += "data: [DONE]\n\n";
    return out;
}

QString transcriptText(AiManager *ai)
{
    QStringList parts;
    for (const QVariant &e : ai->transcript())
        parts.append(e.toMap().value(QStringLiteral("role")).toString() + ':' + e.toMap().value(QStringLiteral("text")).toString());
    return parts.join(QStringLiteral(" | "));
}

} // namespace

void runAiAutoTest(QQuickWindow *window, const Check &check)
{
    // --- SSE parser ---------------------------------------------------------
    {
        const QByteArray stream = ": keep-alive\r\nevent: a\r\ndata: one\r\n\r\ndata: x\ndata: y\n\nid: 7\ndata: last\n\n";
        SseParser whole, bytewise;
        const QList<SseParser::Event> all = whole.feed(stream);
        QList<SseParser::Event> piecewise;
        for (char c : stream)
            piecewise += bytewise.feed(QByteArray(1, c));
        const auto same = [](const QList<SseParser::Event> &a) {
            return a.size() == 3 && a[0].event == QLatin1String("a") && a[0].data == "one" && a[1].data == "x\ny"
                && a[2].data == "last";
        };
        check(same(all) && same(piecewise),
              "SSE parser: CRLF, comments, multi-line data, named events; same result fed byte by byte");
    }

    // --- PKCE: RFC 7636 appendix B ------------------------------------------
    check(OAuth::pkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
          "PKCE S256 challenge matches RFC 7636's worked example");
    check(OAuth::randomToken() != OAuth::randomToken() && OAuth::randomToken().size() == 43,
          "random tokens: 32 bytes, base64url, distinct");

    // --- JWT verification -------------------------------------------------
    TestKey key;
    {
        const QByteArray good = key.sign({{QStringLiteral("sub"), QStringLiteral("u1")}});
        QString why;
        const auto token = Jwt::decode(good);
        check(token && Jwt::verifyRs256(*token, key.jwks(), &why), "JWT: a correctly signed RS256 token verifies " + why);
        QList<QByteArray> parts = good.split('.');
        parts[1] = OAuth::base64Url(R"({"sub":"attacker"})");
        const auto tampered = Jwt::decode(parts.join('.'));
        check(tampered && !Jwt::verifyRs256(*tampered, key.jwks(), &why), "JWT: an altered payload fails (" + why + ")");
        const auto unknownKid = Jwt::decode(key.sign({{QStringLiteral("sub"), QStringLiteral("u1")}}, QStringLiteral("other")));
        check(unknownKid && !Jwt::verifyRs256(*unknownKid, key.jwks(), &why), "JWT: an unknown key id fails (" + why + ")");
        TestKey otherKey;
        const auto wrongKey = Jwt::decode(otherKey.sign({{QStringLiteral("sub"), QStringLiteral("u1")}}));
        check(wrongKey && !Jwt::verifyRs256(*wrongKey, key.jwks(), &why), "JWT: a token signed by another key fails (" + why + ")");
    }

    // --- Mock servers and the window's AiManager ------------------------------
    MockServer server;
    check(server.start(), "mock server listening");
    auto *ai = window->findChild<AiManager *>(QStringLiteral("ai"));
    check(ai != nullptr, "found the window's AiManager");
    if (!ai)
        return;
    AiEndpoints endpoints;
    endpoints.openAiAuth = QUrl(server.base());
    endpoints.openAiApi = QUrl(server.base() + QStringLiteral("/v1"));
    endpoints.openRouter = QUrl(server.base() + QStringLiteral("/or"));
    Browser browser;
    const auto opener = [&browser](const QUrl &url) {
        browser.opened.append(url);
        return true;
    };
    auto storage = std::make_unique<MemoryAiStorage>();
    MemoryAiStorage *store = storage.get();
    ai->configure(std::move(storage), endpoints, opener);
    check(!ai->isReady() && ai->activeProviderId().isEmpty(), "fresh storage: nothing signed in, no active provider");

    // ChatGPT's servers. The token endpoint answers per grant; `next*`
    // let each test change what it returns.
    const QString clientId = QStringLiteral("oaiapp_test");
    QString expectNonce;
    QString nextNonce; // overrides the nonce put in the ID token
    QString nextScope = QStringLiteral("openid profile email offline_access resource.invoke chatgpt.tokens.use.direct");
    QJsonObject nextRefresh; // the refresh grant's response; empty = success
    int refreshCount = 0;
    server.routes.insert(QStringLiteral("GET /.well-known/jwks.json"), [&](const HttpRequest &) { return json(key.jwks()); });
    server.routes.insert(QStringLiteral("POST /api/accounts/oauth/token"), [&](const HttpRequest &r) {
        const QUrlQuery form = r.form();
        if (form.queryItemValue(QStringLiteral("grant_type")) == QLatin1String("refresh_token")) {
            ++refreshCount;
            if (!nextRefresh.isEmpty())
                return json(nextRefresh, 400);
            return json({{QStringLiteral("access_token"), QStringLiteral("access-%1").arg(refreshCount + 1)},
                         {QStringLiteral("refresh_token"), QStringLiteral("refresh-%1").arg(refreshCount + 1)},
                         {QStringLiteral("expires_in"), 3600},
                         {QStringLiteral("scope"), nextScope}});
        }
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const QJsonObject claims{{QStringLiteral("iss"), server.base()},
                                 {QStringLiteral("aud"), clientId},
                                 {QStringLiteral("sub"), QStringLiteral("user-123")},
                                 {QStringLiteral("email"), QStringLiteral("tim@example.com")},
                                 {QStringLiteral("iat"), double(now)},
                                 {QStringLiteral("exp"), double(now + 3600)},
                                 {QStringLiteral("nonce"), nextNonce.isEmpty() ? expectNonce : nextNonce}};
        return json({{QStringLiteral("access_token"), QStringLiteral("access-1")},
                     {QStringLiteral("refresh_token"), QStringLiteral("refresh-1")},
                     {QStringLiteral("id_token"), QString::fromLatin1(key.sign(claims))},
                     {QStringLiteral("token_type"), QStringLiteral("Bearer")},
                     {QStringLiteral("expires_in"), 3600},
                     {QStringLiteral("scope"), nextScope}});
    });
    server.routes.insert(QStringLiteral("POST /api/accounts/oauth/revoke"), [](const HttpRequest &) { return HttpResponse{200, "text/plain", ""}; });
    server.routes.insert(QStringLiteral("GET /v1/models"), [](const HttpRequest &) {
        return json({{QStringLiteral("models"),
                      QJsonArray{QJsonObject{{QStringLiteral("slug"), QStringLiteral("gpt-test")},
                                             {QStringLiteral("display_name"), QStringLiteral("GPT Test")},
                                             {QStringLiteral("visibility"), QStringLiteral("list")}},
                                 QJsonObject{{QStringLiteral("slug"), QStringLiteral("gpt-hidden")},
                                             {QStringLiteral("display_name"), QStringLiteral("Hidden")},
                                             {QStringLiteral("visibility"), QStringLiteral("hide")}}}}});
    });
    QByteArray nextResponses; // body for POST /v1/responses ("" = a good stream)
    int responsesStatus = 200;
    server.routes.insert(QStringLiteral("POST /v1/responses"), [&](const HttpRequest &) {
        if (responsesStatus != 200)
            return HttpResponse{responsesStatus, "application/json", nextResponses};
        return sse(!nextResponses.isEmpty() ? nextResponses
                   : "event: response.created\ndata: {\"type\":\"response.created\"}\n\n"
                     "event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}\n\n"
                     "event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\",\"delta\":\", Tim.\"}\n\n"
                     "event: response.completed\ndata: {\"type\":\"response.completed\"}\n\n");
    });

    AiProvider *chatgpt = ai->chatgpt();
    int lastRedirectStatus = 0; // what the browser got back from Rune's redirect page
    // One ChatGPT sign-in: returns the authorize URL the browser got.
    const auto signInChatGpt = [&](const QList<std::pair<QString, QString>> &extra = {}, bool tamperState = false) {
        browser.opened.clear();
        ai->signIn(QStringLiteral("chatgpt"));
        const QUrl url = browser.opened.value(0);
        expectNonce = param(url, "nonce");
        QList<std::pair<QString, QString>> params{{QStringLiteral("code"), QStringLiteral("auth-code")},
                                                  {QStringLiteral("state"), tamperState ? QStringLiteral("forged") : param(url, "state")},
                                                  {QStringLiteral("client_id"), clientId}};
        params += extra;
        lastRedirectStatus = browser.follow(param(url, "redirect_uri"), params);
        waitFor([&] { return chatgpt->state() != AiProvider::State::SigningIn; });
        return url;
    };

    // --- ChatGPT: sign-in ------------------------------------------------------
    {
        const QUrl url = signInChatGpt();
        const QStringList scopes = param(url, "scope").split(' ');
        check(url.path() == QLatin1String("/api/accounts/authorize") && param(url, "client_id") == QLatin1String("dynamic_agent_client")
                  && param(url, "agent_name_hint") == QLatin1String("Rune") && param(url, "ext_agent_host_id").startsWith("urn:uuid:")
                  && param(url, "response_type") == QLatin1String("code") && scopes.contains("chatgpt.tokens.use.direct")
                  && scopes.contains("offline_access") && scopes.contains("openid")
                  && param(url, "resource") == endpoints.openAiApi.toString()
                  && param(url, "code_challenge_method") == QLatin1String("S256") && !param(url, "nonce").isEmpty()
                  && !param(url, "state").isEmpty(),
              "ChatGPT authorize URL: dynamic registration, host id, plan scopes, resource, PKCE S256, state, nonce");
        check(QRegularExpression(QStringLiteral(R"(^http://127\.0\.0\.1:\d+/auth/callback$)")).match(param(url, "redirect_uri")).hasMatch(),
              "redirect URI is 127.0.0.1 loopback with /auth/callback: " + param(url, "redirect_uri"));
        const HttpRequest *exchange = server.last(QStringLiteral("POST /api/accounts/oauth/token"));
        const QUrlQuery form = exchange ? exchange->form() : QUrlQuery();
        check(exchange && form.queryItemValue("grant_type") == QLatin1String("authorization_code")
                  && form.queryItemValue("client_id") == clientId && form.queryItemValue("code") == QLatin1String("auth-code")
                  && form.queryItemValue("redirect_uri", QUrl::FullyDecoded) == param(url, "redirect_uri")
                  && OAuth::pkceChallenge(form.queryItemValue("code_verifier").toLatin1()) == param(url, "code_challenge").toLatin1()
                  && exchange->headers.value("content-type") == "application/x-www-form-urlencoded",
              "code exchange: form-encoded, issued client_id, same redirect_uri, verifier matches the challenge");
        check(chatgpt->state() == AiProvider::State::SignedIn && chatgpt->account() == QLatin1String("tim@example.com")
                  && ai->isReady() && ai->activeProviderId() == QLatin1String("chatgpt"),
              "signed in as the ID token's email; ChatGPT becomes the active provider (" + chatgpt->error() + ")");
        check(lastRedirectStatus == 200, QStringLiteral("the browser got Rune's \"return to Rune\" page (HTTP %1)").arg(lastRedirectStatus));
        check(store->value("chatgpt/clientId") == clientId && store->m_secrets.value("chatgpt/tokens").contains("refresh-1")
                  && !store->m_values.contains("chatgpt/tokens"),
              "client_id saved as a setting; tokens only as a keyring secret");
        check(ai->showPlanNotice(), "first ChatGPT sign-in shows OpenAI's plan-usage notice");
        ai->dismissPlanNotice();
        waitFor([&] { return !ai->models().isEmpty(); });
        check(ai->models().size() == 1 && ai->model() == QLatin1String("gpt-test"),
              "models: only visibility \"list\" entries; the first becomes the model");
    }

    // --- ChatGPT: streaming ------------------------------------------------------
    {
        ai->send(QStringLiteral("Say hello"));
        waitFor([&] { return !ai->isBusy(); });
        check(transcriptText(ai) == QLatin1String("user:Say hello | assistant:Hello, Tim.") && ai->lastError().isEmpty(),
              "streamed reply assembled from deltas: " + transcriptText(ai) + ai->lastError());
        const HttpRequest *r = server.last(QStringLiteral("POST /v1/responses"));
        const QJsonObject body = r ? r->json() : QJsonObject();
        check(r && r->headers.value("authorization") == "Bearer access-1" && body.value("store") == false
                  && body.value("stream") == true && body.value("model") == QLatin1String("gpt-test")
                  && !body.value("instructions").toString().isEmpty() && body.value("input").toArray().size() == 1,
              "Responses request: bearer token, store:false, stream:true, model, instructions, input");

        nextResponses = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"partial\"}\n\n";
        ai->send(QStringLiteral("Again"));
        waitFor([&] { return !ai->isBusy(); });
        check(ai->lastErrorCode() == QLatin1String("interrupted"),
              "a stream ending without response.completed fails: " + ai->lastErrorCode());

        nextResponses = "data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"subscription_sharing_usage_limit_exceeded\"}}}\n\n";
        ai->send(QStringLiteral("More"));
        waitFor([&] { return !ai->isBusy(); });
        check(ai->lastErrorCode() == QLatin1String("usage_limit") && ai->lastError().contains("Usage limit"),
              "response.failed with the usage-limit code → usage_limit (\"" + ai->lastError() + "\")");

        responsesStatus = 403;
        nextResponses = R"({"error":{"code":"subscription_sharing_user_not_eligible","message":"no"}})";
        ai->send(QStringLiteral("More"));
        waitFor([&] { return !ai->isBusy(); });
        check(ai->lastErrorCode() == QLatin1String("not_eligible"), "HTTP 403 not-eligible → not_eligible: " + ai->lastErrorCode());
        responsesStatus = 200;
        nextResponses.clear();
        ai->clearTranscript();
    }

    // --- ChatGPT: refresh ---------------------------------------------------------
    {
        // Expire the access token: the next request refreshes first.
        QJsonObject tokens = QJsonDocument::fromJson(store->m_secrets.value("chatgpt/tokens").toUtf8()).object();
        tokens.insert("expires_at", double(QDateTime::currentSecsSinceEpoch() + 10));
        store->m_secrets.insert("chatgpt/tokens", QString::fromUtf8(QJsonDocument(tokens).toJson()));
        ai->send(QStringLiteral("After expiry"));
        waitFor([&] { return !ai->isBusy(); });
        const HttpRequest *refresh = server.last(QStringLiteral("POST /api/accounts/oauth/token"));
        const QUrlQuery form = refresh ? refresh->form() : QUrlQuery();
        check(refreshCount == 1 && form.queryItemValue("grant_type") == QLatin1String("refresh_token")
                  && form.queryItemValue("refresh_token") == QLatin1String("refresh-1") && form.queryItemValue("client_id") == clientId
                  && server.last(QStringLiteral("POST /v1/responses"))->headers.value("authorization") == "Bearer access-2",
              "near expiry: refreshed (refresh_token grant, client_id), then the request used the new token");
        check(store->m_secrets.value("chatgpt/tokens").contains("refresh-2"), "the rotated refresh token was saved");

        tokens = QJsonDocument::fromJson(store->m_secrets.value("chatgpt/tokens").toUtf8()).object();
        tokens.insert("expires_at", double(QDateTime::currentSecsSinceEpoch()));
        store->m_secrets.insert("chatgpt/tokens", QString::fromUtf8(QJsonDocument(tokens).toJson()));
        nextRefresh = {{QStringLiteral("error"), QStringLiteral("refresh_token_reused")}};
        ai->send(QStringLiteral("After revocation"));
        waitFor([&] { return !ai->isBusy(); });
        check(chatgpt->state() == AiProvider::State::SignedOut && !store->m_secrets.contains("chatgpt/tokens")
                  && store->value("chatgpt/clientId") == clientId && !ai->isReady()
                  && ai->lastErrorCode() == QLatin1String("signed_out"),
              "terminal refresh error: signed out, tokens forgotten, client_id kept (" + chatgpt->error() + ")");
        nextRefresh = {};
        ai->clearTranscript();
    }

    // --- ChatGPT: returning sign-in, rejected sign-ins, sign-out --------------------
    {
        const QUrl url = signInChatGpt();
        check(param(url, "client_id") == clientId && !QUrlQuery(url).hasQueryItem("agent_name_hint")
                  && param(url, "login_hint") == QLatin1String("tim@example.com") && chatgpt->state() == AiProvider::State::SignedIn,
              "returning sign-in reuses the issued client_id, no agent_name_hint, login_hint set");
        check(!ai->showPlanNotice(), "the plan notice isn't shown again");

        ai->signOut(QStringLiteral("chatgpt"));
        waitFor([&] { return server.count(QStringLiteral("POST /api/accounts/oauth/revoke")) > 0; });
        const HttpRequest *revoke = server.last(QStringLiteral("POST /api/accounts/oauth/revoke"));
        check(revoke && revoke->form().queryItemValue("token").startsWith("refresh-")
                  && revoke->form().queryItemValue("token_type_hint") == QLatin1String("refresh_token")
                  && revoke->form().queryItemValue("client_id") == clientId,
              "sign-out revokes the refresh token at the revocation endpoint");
        check(chatgpt->state() == AiProvider::State::SignedOut && !store->m_secrets.contains("chatgpt/tokens")
                  && store->value("chatgpt/clientId") == clientId && !store->value("chatgpt/hostId").toString().isEmpty(),
              "after sign-out: tokens gone; client_id and host id kept for next time");

        nextNonce = QStringLiteral("replayed-nonce");
        signInChatGpt();
        check(chatgpt->state() == AiProvider::State::SignedOut && chatgpt->error().contains("nonce mismatch")
                  && !store->m_secrets.contains("chatgpt/tokens"),
              "an ID token with the wrong nonce is rejected: " + chatgpt->error());
        nextNonce.clear();

        signInChatGpt({}, true);
        check(chatgpt->state() == AiProvider::State::SignedOut && chatgpt->error().contains("didn't match"),
              "a redirect with the wrong state is rejected before any code exchange: " + chatgpt->error());

        nextScope = QStringLiteral("openid profile email");
        signInChatGpt();
        check(chatgpt->state() == AiProvider::State::SignedOut && chatgpt->error().contains("allow"),
              "signing in without granting plan usage doesn't count as signed in: " + chatgpt->error());
        nextScope = QStringLiteral("openid profile email offline_access resource.invoke chatgpt.tokens.use.direct");

        browser.opened.clear();
        ai->signIn(QStringLiteral("chatgpt"));
        const QUrl denied = browser.opened.value(0);
        browser.follow(param(denied, "redirect_uri"), {{QStringLiteral("error"), QStringLiteral("access_denied")},
                                                       {QStringLiteral("state"), param(denied, "state")}});
        waitFor([&] { return chatgpt->state() != AiProvider::State::SigningIn; });
        check(chatgpt->state() == AiProvider::State::SignedOut && chatgpt->error() == QLatin1String("Sign-in was cancelled."),
              "declining on ChatGPT's page: \"" + chatgpt->error() + "\"");
    }

    // --- OpenRouter ----------------------------------------------------------------
    AiProvider *openrouter = ai->openrouter();
    server.routes.insert(QStringLiteral("POST /or/api/v1/auth/keys"), [](const HttpRequest &r) {
        const QJsonObject body = r.json();
        return json({{QStringLiteral("key"), QStringLiteral("sk-or-test-") + body.value("code").toString()},
                     {QStringLiteral("verifier"), body.value("code_verifier")}});
    });
    server.routes.insert(QStringLiteral("GET /or/api/v1/models"), [](const HttpRequest &) {
        QJsonArray data;
        for (const char *id : {"meta/llama-x", "anthropic/claude-opus-x", "anthropic/claude-sonnet-x", "anthropic/claude-sonnet-x:batch",
                               "openai/gpt-x", "x-ai/grok-x"})
            data.append(QJsonObject{{QStringLiteral("id"), QString::fromLatin1(id)}, {QStringLiteral("name"), QString::fromLatin1(id).toUpper()}});
        return json({{QStringLiteral("data"), data}});
    });
    int chatStatus = 200;
    QByteArray nextChat; // the chat stream body; "" = the default "Hi there"
    bool chatHoldOpen = false; // leave that stream open (see HttpResponse::holdOpen)
    server.routes.insert(QStringLiteral("POST /or/api/v1/chat/completions"), [&](const HttpRequest &) {
        if (chatStatus != 200)
            return HttpResponse{chatStatus, "application/json", R"({"error":{"message":"nope"}})"};
        if (!nextChat.isEmpty()) {
            HttpResponse response = sse(nextChat);
            response.holdOpen = chatHoldOpen;
            return response;
        }
        return sse(": OPENROUTER PROCESSING\n\n"
                   "data: {\"choices\":[{\"delta\":{\"content\":\"Hi \"}}]}\n\n"
                   "data: {\"choices\":[{\"delta\":{\"content\":\"there\"},\"finish_reason\":\"stop\"}]}\n\n"
                   "data: [DONE]\n\n");
    });
    {
        browser.opened.clear();
        ai->signIn(QStringLiteral("openrouter"));
        const QUrl url = browser.opened.value(0);
        const QString callback = param(url, "callback_url");
        check(url.path() == QLatin1String("/or/auth")
                  && QRegularExpression(QStringLiteral(R"(^http://localhost:\d+/callback$)")).match(callback).hasMatch()
                  && param(url, "code_challenge_method") == QLatin1String("S256") && !param(url, "code_challenge").isEmpty()
                  && !param(url, "state").isEmpty(),
              "OpenRouter auth URL: localhost callback, PKCE S256, state: " + callback);
        // Through "localhost", which may resolve to IPv6 or IPv4.
        const int orStatus = browser.follow(callback, {{QStringLiteral("code"), QStringLiteral("or-code")}, {QStringLiteral("state"), param(url, "state")}});
        check(orStatus == 200, QStringLiteral("OpenRouter redirect via localhost answered (HTTP %1)").arg(orStatus));
        waitFor([&] { return openrouter->state() != AiProvider::State::SigningIn; });
        const HttpRequest *exchange = server.last(QStringLiteral("POST /or/api/v1/auth/keys"));
        check(exchange && exchange->json().value("code") == QLatin1String("or-code")
                  && exchange->json().value("code_challenge_method") == QLatin1String("S256")
                  && OAuth::pkceChallenge(exchange->json().value("code_verifier").toString().toLatin1())
                         == param(url, "code_challenge").toLatin1(),
              "OpenRouter key exchange: JSON with code, verifier matching the challenge, S256");
        check(openrouter->state() == AiProvider::State::SignedIn
                  && store->m_secrets.value("openrouter/key") == QLatin1String("sk-or-test-or-code"),
              "OpenRouter signed in; key kept as a keyring secret (" + openrouter->error() + ")");
        check(ai->activeProviderId() == QLatin1String("openrouter"),
              "with ChatGPT signed out, the newly signed-in OpenRouter becomes active");
        waitFor([&] { return !ai->models().isEmpty(); });
        QStringList ids;
        for (const QVariant &m : ai->models())
            ids.append(m.toMap().value("id").toString());
        check(ids == QStringList{"anthropic/claude-opus-x", "anthropic/claude-sonnet-x", "openai/gpt-x", "x-ai/grok-x"}
                  && ai->model() == QLatin1String("anthropic/claude-sonnet-x"),
              "models limited to Claude/GPT/Grok families, no :variants; default is Claude Sonnet: " + ids.join(", "));

        ai->send(QStringLiteral("Hello"));
        waitFor([&] { return !ai->isBusy(); });
        const HttpRequest *chat = server.last(QStringLiteral("POST /or/api/v1/chat/completions"));
        const QJsonArray messages = chat ? chat->json().value("messages").toArray() : QJsonArray();
        check(transcriptText(ai) == QLatin1String("user:Hello | assistant:Hi there") && chat
                  && chat->headers.value("authorization") == "Bearer sk-or-test-or-code"
                  && chat->json().value("stream") == true && messages.size() == 2
                  && messages[0].toObject().value("role") == QLatin1String("system"),
              "OpenRouter stream: comment line skipped, deltas assembled, [DONE] ends it; system + user messages: "
                  + transcriptText(ai));

        ai->setModel(QStringLiteral("x-ai/grok-x"));
        ai->send(QStringLiteral("Second"));
        waitFor([&] { return !ai->isBusy(); });
        check(server.last(QStringLiteral("POST /or/api/v1/chat/completions"))->json().value("model") == QLatin1String("x-ai/grok-x")
                  && server.last(QStringLiteral("POST /or/api/v1/chat/completions"))->json().value("messages").toArray().size() == 4
                  && store->value("ai/model/openrouter") == QLatin1String("x-ai/grok-x"),
              "the chosen model is used and remembered; history is sent with the new prompt");

        chatStatus = 402;
        ai->send(QStringLiteral("Third"));
        waitFor([&] { return !ai->isBusy(); });
        check(ai->lastErrorCode() == QLatin1String("credits") && openrouter->state() == AiProvider::State::SignedIn,
              "HTTP 402 → credits error, still signed in");
        chatStatus = 200;

        // A restart: new providers over the same storage.
        auto reloaded = std::make_unique<MemoryAiStorage>(*store);
        MemoryAiStorage *reloadedStore = reloaded.get();
        ai->configure(std::move(reloaded), endpoints, opener);
        store = reloadedStore;
        openrouter = ai->openrouter();
        chatgpt = ai->chatgpt();
        check(openrouter->state() == AiProvider::State::SignedIn && ai->activeProviderId() == QLatin1String("openrouter")
                  && chatgpt->state() == AiProvider::State::SignedOut,
              "after a restart: still signed in to OpenRouter, still active");
        check(ai->model() == QLatin1String("x-ai/grok-x"), "... and the chosen model is remembered before the list loads");

        chatStatus = 401;
        ai->send(QStringLiteral("Revoked?"));
        waitFor([&] { return !ai->isBusy(); });
        check(openrouter->state() == AiProvider::State::SignedOut && !store->m_secrets.contains("openrouter/key")
                  && ai->lastErrorCode() == QLatin1String("signed_out"),
              "HTTP 401 (key deleted on OpenRouter) → signed out, key forgotten");
        chatStatus = 200;

        // Sign in again, then sign out: the key's OpenRouter page is offered.
        browser.opened.clear();
        ai->signIn(QStringLiteral("openrouter"));
        const QUrl again = browser.opened.value(0);
        browser.follow(param(again, "callback_url"), {{QStringLiteral("code"), QStringLiteral("c2")}, {QStringLiteral("state"), param(again, "state")}});
        waitFor([&] { return openrouter->state() != AiProvider::State::SigningIn; });
        ai->signOut(QStringLiteral("openrouter"));
        const QString hash = QString::fromLatin1(QCryptographicHash::hash("sk-or-test-c2", QCryptographicHash::Sha256).toHex());
        check(openrouter->state() == AiProvider::State::SignedOut && !store->m_secrets.contains("openrouter/key")
                  && openrouter->errorLink() == QLatin1String("https://openrouter.ai/keys/") + hash,
              "OpenRouter sign-out forgets the key and links to its page to delete it");

        browser.opened.clear();
        ai->signIn(QStringLiteral("openrouter"));
        const QUrl declined = browser.opened.value(0);
        browser.follow(param(declined, "callback_url"), {});
        waitFor([&] { return openrouter->state() != AiProvider::State::SigningIn; });
        check(openrouter->state() == AiProvider::State::SignedOut && openrouter->error() == QLatin1String("Sign-in was cancelled."),
              "declining on OpenRouter's page: \"" + openrouter->error() + "\"");
    }


    // --- The document: Ask sees it, Edit changes it ------------------------------------
    {
        auto *controller = window->findChild<DocumentController *>(QStringLiteral("controller"));
        QTextDocument *doc = controller ? controller->textDocument() : nullptr;
        check(controller && ai->document() == controller, "the panel's AiManager is wired to the document");
        if (!controller)
            return;
        browser.opened.clear();
        ai->signIn(QStringLiteral("openrouter"));
        const QUrl url = browser.opened.value(0);
        browser.follow(param(url, "callback_url"), {{QStringLiteral("code"), QStringLiteral("doc")}, {QStringLiteral("state"), param(url, "state")}});
        waitFor([&] { return openrouter->state() != AiProvider::State::SigningIn; });
        waitFor([&] { return !ai->models().isEmpty(); });
        ai->clearTranscript();
        const auto lastChat = [&] { return server.last(QStringLiteral("POST /or/api/v1/chat/completions"))->json(); };
        const auto lastUserMessage = [&] {
            const QJsonArray m = lastChat().value("messages").toArray();
            return m.last().toObject().value("content").toString();
        };
        const auto run = [&](const std::function<bool()> &start) {
            const bool started = start();
            waitFor([&] { return !ai->isBusy(); });
            return started;
        };

        // Ask: the document and the selection go along as context.
        controller->newDocument();
        doc->setPlainText(QStringLiteral("Alpha paragraph about otters.\nBeta paragraph."));
        controller->setSelection(15, 15, 21); // "about "
        run([&] { return ai->send(QStringLiteral("What is this about?")); });
        const QString system = lastChat().value("messages").toArray().first().toObject().value("content").toString();
        check(system.contains("Alpha paragraph") && system.contains("<selection>") && system.contains("Beta paragraph")
                  && lastUserMessage() == QLatin1String("What is this about?") && doc->toPlainText().startsWith("Alpha"),
              "Ask: the document and selection go along as context; the answer stays in the panel");

        // Edit, nothing selected: streams in at the cursor, then becomes formatting.
        controller->newDocument();
        doc->setPlainText(QStringLiteral("Intro."));
        doc->setModified(false);
        const QString beforeHtml = doc->toHtml();
        const int undoBefore = doc->availableUndoSteps();
        controller->setSelection(6, 6, 6);
        nextChat = chatStream({"# Plan\n\nWe will **ship** it.\n\n", "- first\n- second\n\n| A | B |\n|---|---|\n| 1 | 2 |\n"});
        bool readOnlyDuring = false;
        QObject::connect(controller, &DocumentController::streamingEditChanged, ai, [&] {
            readOnlyDuring = readOnlyDuring || controller->isStreamingEdit();
        });
        run([&] { return ai->edit(QStringLiteral("Add a short plan")); });
        const QString sent = lastUserMessage();
        check(sent.contains("Intro.") && sent.contains("<cursor/>") && sent.endsWith("Instruction: Add a short plan")
                  && lastChat().value("messages").toArray().first().toObject().value("content").toString().contains("<<<<<<< FIND"),
              "Edit: the request carries the document around <cursor/> and the instruction, with the edit instructions");
        QTextBlock title;
        int lists = 0, boldRuns = 0;
        bool table = false;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            if (b.text() == QLatin1String("Plan"))
                title = b;
            if (b.textList() && b.text() == QLatin1String("first"))
                lists = b.textList()->count();
            for (auto it = b.begin(); !it.atEnd(); ++it)
                if (it.fragment().text() == QLatin1String("ship") && it.fragment().charFormat().fontWeight() >= QFont::Bold)
                    ++boldRuns;
        }
        for (auto it = doc->rootFrame()->begin(); !it.atEnd(); ++it)
            table = table || qobject_cast<QTextTable *>(it.currentFrame());
        check(doc->toPlainText().startsWith("Intro.") && title.isValid() && title.blockFormat().headingLevel() == 1
                  && boldRuns == 1 && lists == 2 && table && !controller->isStreamingEdit() && readOnlyDuring,
              "Edit inserts at the cursor as formatting: heading (level 1, even as the first block), bold, list, table");
        check(transcriptText(ai).endsWith("assistant:Inserted 21 words at the cursor.") && ai->canUndoEdit(),
              "the panel records the edit, not the text: " + transcriptText(ai));
        check(doc->availableUndoSteps() > undoBefore, "the edit is on the undo stack");
        ai->undoLastEdit();
        check(doc->toHtml() == beforeHtml && !controller->isDirty() && !ai->canUndoEdit(),
              "one undo takes the whole AI edit back; the document is exactly as before, and clean");

        // Several paragraphs inserted mid-paragraph get paragraphs of their own.
        controller->newDocument();
        doc->setPlainText(QStringLiteral("Before after"));
        controller->setSelection(6, 6, 6);
        nextChat = chatStream({"## Middle\n\nBody."});
        run([&] { return ai->edit(QStringLiteral("Insert a section")); });
        QStringList blocks;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            blocks.append(QString::number(b.blockFormat().headingLevel()) + ':' + b.text());
        check(blocks == QStringList{"0:Before", "2:Middle", "0:Body.", "0:after"},
              "a multi-paragraph reply mid-paragraph splits it rather than merging into it: " + blocks.join(" | "));
        // A one-line rewrite stays inline.
        controller->newDocument();
        doc->setPlainText(QStringLiteral("One two three"));
        controller->setSelection(7, 4, 7);
        nextChat = chatStream({"2"});
        run([&] { return ai->edit(QStringLiteral("Digits")); });
        check(doc->toPlainText() == QLatin1String("One 2 three") && doc->blockCount() == 1,
              "a single-phrase rewrite stays inside its paragraph: \"" + doc->toPlainText() + "\"");

        // Edit with a selection: the reply replaces it, in one step.
        controller->newDocument();
        doc->setPlainText(QStringLiteral("The quick fox."));
        controller->setSelection(9, 4, 9); // "quick"
        nextChat = chatStream({"sl", "ow"});
        run([&] { return ai->edit(QStringLiteral("Make it slower")); });
        check(doc->toPlainText() == QLatin1String("The slow fox.") && lastUserMessage().contains("<selection>\nquick"),
              "Edit with a selection replaces it: \"" + doc->toPlainText() + "\"");
        controller->undo();
        check(doc->toPlainText() == QLatin1String("The quick fox."), "... and Ctrl+Z restores the original in one step");

        // A reply wrapped in a code fence, despite the instructions.
        controller->setSelection(9, 4, 9);
        nextChat = chatStream({"```markdown\nnimble\n```"});
        run([&] { return ai->edit(QStringLiteral("Synonym")); });
        check(doc->toPlainText() == QLatin1String("The nimble fox."), "a code fence around the whole reply is dropped: \"" + doc->toPlainText() + "\"");

        // Failures revert the document; nothing lands on the undo stack.
        doc->setPlainText(QStringLiteral("Keep me."));
        doc->clearUndoRedoStacks();
        const QString keepHtml = doc->toHtml();
        controller->setSelection(0, 0, 4);
        chatStatus = 402;
        run([&] { return ai->edit(QStringLiteral("Rewrite")); });
        check(doc->toHtml() == keepHtml && !doc->isUndoAvailable() && ai->lastErrorCode() == QLatin1String("credits")
                  && !controller->isStreamingEdit() && transcriptText(ai).endsWith("Failed, so the document wasn't changed."),
              "an edit that fails before writing restores the selected text; no undo step left behind");
        chatStatus = 200;
        controller->setSelection(0, 0, 4);
        nextChat = chatStream({"Half a rep"}, false); // cut off: no [DONE]
        run([&] { return ai->edit(QStringLiteral("Rewrite")); });
        check(doc->toHtml() == keepHtml && !doc->isUndoAvailable() && ai->lastErrorCode() == QLatin1String("interrupted"),
              "an edit cut off mid-stream is reverted, partial text and all: \"" + doc->toPlainText() + "\"");

        // Stop keeps what arrived; it's still one undo step.
        controller->setSelection(8, 8, 8);
        nextChat = chatStream({" More text."}, false);
        chatHoldOpen = true;
        ai->edit(QStringLiteral("Continue"));
        waitFor([&] { return doc->toPlainText().endsWith("More text."); });
        ai->cancel();
        chatHoldOpen = false;
        check(doc->toPlainText() == QLatin1String("Keep me. More text.") && !controller->isStreamingEdit() && ai->canUndoEdit()
                  && transcriptText(ai).endsWith("Stopped. Kept what was written so far."),
              "Stop keeps what was written: \"" + doc->toPlainText() + "\"");
        ai->undoLastEdit();
        check(doc->toPlainText() == QLatin1String("Keep me."), "... and Undo takes it back in one step");

        // While the AI writes, typing and undo are refused.
        controller->setSelection(8, 8, 8);
        check(controller->beginStreamedEdit(), "a streamed edit can start");
        controller->appendStreamedText(QStringLiteral(" AI"));
        const bool typed = controller->typeText(QStringLiteral("x"));
        controller->undo();
        const QString during = doc->toPlainText();
        controller->abortStreamedEdit(false);
        check(!typed && during == QLatin1String("Keep me. AI") && doc->toPlainText() == QLatin1String("Keep me."),
              "during a streamed edit typing and undo are refused; aborting reverts it");

        // Typing after an edit means "Undo this edit" would undo the typing instead.
        nextChat = chatStream({"Added."});
        controller->setSelection(8, 8, 8);
        run([&] { return ai->edit(QStringLiteral("Add")); });
        const bool couldUndo = ai->canUndoEdit();
        QTextCursor(doc).insertText(QStringLiteral("typed "));
        check(couldUndo && !ai->canUndoEdit(), "\"Undo this edit\" is withdrawn once the document changes otherwise");


        // Changes to existing text: find/replace blocks, applied in place.
        const auto change = [](const QString &find, const QString &replace) {
            return QStringLiteral("<<<<<<< FIND\n") + find + QStringLiteral("\n=======\n") + replace + QStringLiteral("\n>>>>>>> REPLACE\n");
        };
        controller->newDocument();
        doc->setPlainText(QStringLiteral("Teh cat sat on teh mat. It was hapy."));
        {
            QTextCursor bold(doc);
            bold.setPosition(4);
            bold.setPosition(7, QTextCursor::KeepAnchor);
            QTextCharFormat f;
            f.setFontWeight(QFont::Bold);
            bold.mergeCharFormat(f);
        }
        doc->clearUndoRedoStacks();
        const QString misspelt = doc->toHtml();
        controller->setSelection(0, 0, 0);
        nextChat = chatStream({change("Teh cat", "The cat"), change("on teh mat", "on the mat") + change("not in the document", "x"),
                               change("was hapy", "was happy")});
        run([&] { return ai->edit(QStringLiteral("Fix the spelling")); });
        QTextCursor catAt(doc);
        catAt.setPosition(6);
        check(doc->toPlainText() == QLatin1String("The cat sat on the mat. It was happy.") && catAt.charFormat().fontWeight() >= QFont::Bold,
              "spell-check with nothing selected corrects the whole document, keeping formatting (\"cat\" still bold): \""
                  + doc->toPlainText() + "\"");
        check(transcriptText(ai).endsWith("Made 3 changes. 1 change was skipped: its text wasn't found.") && ai->canUndoEdit(),
              "the panel reports changes made and skipped: " + transcriptText(ai).section(" | ", -1));
        ai->undoLastEdit();
        check(doc->toHtml() == misspelt, "one undo takes back all of the changes");

        // Applied as each block completes, not at the end.
        controller->setSelection(0, 0, 0);
        nextChat = chatStream({change("Teh cat", "The cat")}, false);
        chatHoldOpen = true;
        ai->edit(QStringLiteral("Fix"));
        const bool live = waitFor([&] { return doc->toPlainText().startsWith("The cat"); }, 3000) && ai->isBusy();
        ai->cancel();
        chatHoldOpen = false;
        check(live && doc->toPlainText().startsWith("The cat") && ai->canUndoEdit(),
              "each change lands while the reply is still streaming; Stop keeps it");
        ai->undoLastEdit();

        // With a selection, only the selection changes.
        doc->setPlainText(QStringLiteral("teh one. teh two."));
        controller->setSelection(17, 9, 17);
        nextChat = chatStream({change("teh", "the")});
        run([&] { return ai->edit(QStringLiteral("Fix")); });
        check(doc->toPlainText() == QLatin1String("teh one. the two."), "changes stay inside the selection: \"" + doc->toPlainText() + "\"");

        // "Leave a line between them": an empty paragraph, not a drawn line, even in a list.
        controller->newDocument();
        doc->setPlainText(QStringLiteral("Apples\nPears"));
        controller->setSelection(0, 0, 12);
        controller->toggleBulletList();
        controller->setSelection(0, 0, 0);
        nextChat = chatStream({change("Apples\nPears", "Apples\n\nPears")});
        run([&] { return ai->edit(QStringLiteral("Leave a line between the two items")); });
        QStringList shape;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            shape.append((b.textList() ? "•" : "") + b.text());
        check(shape == QStringList{"•Apples", "", "•Pears"},
              "a blank line between list items is an empty paragraph outside the list: " + shape.join(" | "));

        // New text: "&nbsp;" is an empty paragraph, no horizontal rule.
        controller->newDocument();
        nextChat = chatStream({"One\n\n&nbsp;\n\nTwo"});
        run([&] { return ai->edit(QStringLiteral("Two lines with a gap")); });
        QStringList gap;
        bool rule = false;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            gap.append(b.text());
            rule = rule || b.blockFormat().hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth);
        }
        check(gap == QStringList{"One", "", "Two"} && !rule, "&nbsp; in new text becomes an empty paragraph: " + gap.join(" | "));

        // Quotes with Markdown markup or different spacing still match.
        controller->newDocument();
        doc->setPlainText(QStringLiteral("A bold   claim here."));
        controller->setSelection(0, 0, 0);
        nextChat = chatStream({change("A **bold** claim", "A strong claim")});
        run([&] { return ai->edit(QStringLiteral("Soften")); });
        check(doc->toPlainText() == QLatin1String("A strong claim here."),
              "a FIND quoting Markdown markup and collapsed spaces still matches: \"" + doc->toPlainText() + "\"");

        // Nothing found: the document is untouched and nothing to undo.
        doc->clearUndoRedoStacks();
        const QString untouched = doc->toHtml();
        nextChat = chatStream({change("words that are not there", "x")});
        run([&] { return ai->edit(QStringLiteral("Fix")); });
        check(doc->toHtml() == untouched && !doc->isUndoAvailable() && !ai->canUndoEdit()
                  && transcriptText(ai).endsWith("Couldn't find the text the AI wanted to change, so the document wasn't changed."),
              "changes whose text isn't found leave the document untouched");

        // AI output saves and reopens: list then table goes through the sanitizer.
        controller->newDocument();
        controller->setSelection(0, 0, 0);
        nextChat = chatStream({"Notes:\n\n- a\n- b\n\n| X | Y |\n|---|---|\n| 1 | 2 |\n"});
        run([&] { return ai->edit(QStringLiteral("Table")); });
        const QString path = QStringLiteral("/tmp/rune-native-ai-edit.odt");
        QTextDocument reopened;
        QString readError;
        const bool saved = controller->saveToOdf(path);
        const bool read = OdfReader::readInto(&reopened, path, &readError);
        bool reopenedTable = false;
        for (auto it = reopened.rootFrame()->begin(); !it.atEnd(); ++it)
            if (auto *t = qobject_cast<QTextTable *>(it.currentFrame()))
                reopenedTable = t->rows() == 2 && t->cellAt(1, 1).firstCursorPosition().block().text() == QLatin1String("2");
        check(saved && read && reopenedTable && reopened.toPlainText().contains("Notes:"),
              "AI-written list + table saves to .odt and reads back with the table intact " + readError);
        nextChat.clear();
        ai->clearTranscript();
        controller->newDocument();
    }

    // --- Loopback hygiene and the panel ----------------------------------------------
    {
        OAuthLoopback loopback;
        loopback.listen(QStringLiteral("127.0.0.1"), QStringLiteral("/cb"));
        int received = 0;
        QObject::connect(&loopback, &OAuthLoopback::received, [&](const QUrlQuery &) { ++received; });
        const QString uri = loopback.redirectUri();
        const int favicon = browser.follow(QString(uri).replace("/cb", "/favicon.ico"), {});
        const int first = browser.follow(uri, {{QStringLiteral("code"), QStringLiteral("x")}});
        check(favicon == 404 && first == 200 && received == 1,
              QStringLiteral("loopback: other paths 404, the callback is answered once (%1, %2, %3 received)")
                  .arg(favicon).arg(first).arg(received));
        QNetworkReply *after = browser.network.get(QNetworkRequest(QUrl(uri)));
        waitFor([after] { return after->isFinished(); });
        check(after->error() != QNetworkReply::NoError && received == 1, "loopback stops listening after the redirect");
        after->deleteLater();

        QQuickItem *panel = window->findChild<QQuickItem *>(QStringLiteral("aiPanel"));
        QQuickItem *aiButton = window->findChild<QQuickItem *>(QStringLiteral("aiButton"));
        QMetaObject::invokeMethod(aiButton, "clicked");
        QCoreApplication::processEvents();
        QQuickItem *signIn = window->findChild<QQuickItem *>(QStringLiteral("chatgptSignIn"));
        check(panel && panel->isVisible() && signIn && signIn->isVisible(),
              "toolbar AI button opens the panel, with \"Continue with ChatGPT\" while signed out");
        QMetaObject::invokeMethod(aiButton, "clicked");
        QCoreApplication::processEvents();
        check(panel && !panel->isVisible(), "... and closes it");
    }

    // Leave the window's manager clean for anything after.
    ai->configure(std::make_unique<MemoryAiStorage>(), AiEndpoints(), [](const QUrl &) { return false; });
}
