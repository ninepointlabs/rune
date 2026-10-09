#pragma once

// A source of model responses that the user signs in to: ChatGptProvider
// (the user's ChatGPT plan, via Sign in with ChatGPT) and
// OpenRouterProvider (an OpenRouter key from its OAuth flow). AiManager is
// what QML talks to; providers are exposed to it for state and models.
//
// Sign-in is asynchronous and browser-based: signIn() opens the provider's
// page with AiProvider::openBrowser and listens for the redirect (see
// OAuthLoopback); `state` goes SigningIn → SignedIn, or back to SignedOut
// with `error` saying why. A signed-in provider stays signed in across
// restarts (AiStorage); its secrets are read from the keyring only when a
// request needs them.

#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <functional>

#include "SseParser.h"

class AiStorage;
class QNetworkReply;

// Where each provider's servers are. The defaults are production; tests
// point them at local mock servers.
struct AiEndpoints
{
    // auth.openai.com, from its /.well-known/openid-configuration
    // (issuer; /api/accounts/authorize, /oauth/token, /oauth/revoke;
    // /.well-known/jwks.json).
    QUrl openAiAuth = QUrl(QStringLiteral("https://auth.openai.com"));
    // Responses and models API; also the OAuth `resource`.
    QUrl openAiApi = QUrl(QStringLiteral("https://api.openai.com/v1"));
    // /auth (sign-in page) and /api/v1/... (keys, models, chat).
    QUrl openRouter = QUrl(QStringLiteral("https://openrouter.ai"));
};

struct AiMessage
{
    QString role; // "user" or "assistant"
    QString text;
};

struct AiRequest
{
    QString model;
    QString instructions; // system prompt; may be ""
    QList<AiMessage> messages;
};

// One streamed response. Emits delta() for each piece of text, then exactly
// one of finished() or failed(); cancel() stops it with neither. Owned by
// whoever asked for it.
class AiStream : public QObject
{
    Q_OBJECT

public:
    // Turns one server-sent event into progress. Returns false once the
    // stream is complete (finished() or failed() was emitted).
    using EventHandler = std::function<bool(AiStream *, const SseParser::Event &)>;
    // Turns an HTTP error (status >= 400 before the stream starts) into
    // failed(): gets the status and the body.
    using HttpErrorHandler = std::function<void(AiStream *, int, const QByteArray &)>;

    explicit AiStream(QObject *parent = nullptr) : QObject(parent) {}
    ~AiStream() override;

    // Reads `reply` as an event stream. A stream that ends before an
    // EventHandler reports completion fails ("interrupted").
    void start(QNetworkReply *reply, EventHandler onEvent, HttpErrorHandler onHttpError);
    void cancel();
    bool isDone() const { return m_done; }

    void emitDelta(const QString &text) { emit delta(text); }
    // Each emits at most once per stream, and only if not already done.
    void finish();
    void fail(const QString &code, const QString &message);

signals:
    void delta(const QString &text);
    void finished();
    // `code`: a stable identifier the UI can act on ("usage_limit",
    // "credits", "signed_out", "interrupted", "network", or the provider's
    // own); `message`: for the user.
    void failed(const QString &code, const QString &message);

private:
    void onReadyRead();
    void onReplyFinished();

    QPointer<QNetworkReply> m_reply;
    SseParser m_parser;
    EventHandler m_onEvent;
    HttpErrorHandler m_onHttpError;
    QByteArray m_errorBody;
    bool m_done = false;
};

class AiProvider : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Providers are owned by AiManager.")
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    // Who is signed in, for display ("" if unknown).
    Q_PROPERTY(QString account READ account NOTIFY stateChanged)
    // Why the last sign-in or sign-out failed, or a notice about it; "" if none.
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // A page that helps with `error` (e.g. where to delete a key); "" if none.
    Q_PROPERTY(QString errorLink READ errorLink NOTIFY stateChanged)
    // [{id, name}], once signed in and fetched.
    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)

public:
    enum class State { SignedOut, SigningIn, SignedIn };
    Q_ENUM(State)

    using BrowserOpener = std::function<bool(const QUrl &)>;

    AiProvider(AiStorage *storage, const AiEndpoints &endpoints, BrowserOpener openBrowser, QObject *parent);

    virtual QString id() const = 0;
    virtual QString name() const = 0;
    State state() const { return m_state; }
    QString account() const { return m_account; }
    QString error() const { return m_error; }
    QString errorLink() const { return m_errorLink; }
    QVariantList models() const { return m_models; }

    virtual void signIn() = 0;
    // Abandons a sign-in in progress (the browser page is left as it is).
    virtual void cancelSignIn();
    virtual void signOut() = 0;
    virtual void refreshModels() = 0;
    // The model to use when the user hasn't chosen one ("" if none known).
    virtual QString defaultModel() const;
    // Starts a streamed response; the caller owns the result. Never null.
    virtual AiStream *stream(const AiRequest &request) = 0;

    // How long a sign-in may wait for the browser before giving up.
    static constexpr int kSignInTimeoutMs = 5 * 60 * 1000;

signals:
    void stateChanged();
    void modelsChanged();

protected:
    void setState(State state, const QString &error = QString(), const QString &errorLink = QString());
    void setAccount(const QString &account);
    void setModels(const QVariantList &models);
    // JSON body of `reply` (empty object if it isn't one).
    static QJsonObject jsonBody(const QByteArray &body);

    AiStorage *m_storage;
    AiEndpoints m_endpoints;
    BrowserOpener m_openBrowser;
    QNetworkAccessManager m_network;

private:
    State m_state = State::SignedOut;
    QString m_account;
    QString m_error;
    QString m_errorLink;
    QVariantList m_models;
};
