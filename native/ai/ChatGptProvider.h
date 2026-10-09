#pragma once

// ChatGPT plan usage through "Sign in with ChatGPT" for open-source, locally
// run apps (developers.openai.com/siwc/token-sharing-open-source, as of
// 2026-10-09). Eligible Plus/Pro users authorize Rune once in the browser;
// requests then draw on their ChatGPT plan, not an API key.
//
// - Registration and sign-in: a public OAuth client with PKCE (S256),
//   state and nonce, redirected to http://127.0.0.1:<port>/auth/callback.
//   The first sign-in registers dynamically (client_id
//   "dynamic_agent_client"); the client_id issued in the callback is saved
//   and reused. The persistent, opaque ext_agent_host_id is a urn:uuid.
// - The ID token is verified (RS256 against OpenAI's JWKS; iss, aud, exp,
//   nonce) and the chatgpt.tokens.use.direct scope must be granted.
// - Access tokens last an hour: refreshed (rotating the refresh token)
//   when within kRefreshMarginSecs of expiry, one refresh at a time.
//   Terminal refresh errors sign the user out (keeping the client_id).
// - Requests: POST <api>/responses, store:false, stream:true; text from
//   response.output_text.delta, success only at response.completed.
// - Sign-out revokes the refresh token, then forgets the tokens; the
//   client_id and host id are kept for the next sign-in.
//
// Storage: secret "chatgpt/tokens" (JSON: access_token, refresh_token,
// id_token, expires_at, scope); values chatgpt/hostId, chatgpt/clientId,
// chatgpt/account (email), chatgpt/signedIn.

#include "AiProvider.h"
#include "OAuthLoopback.h"

#include <QTimer>

class ChatGptProvider : public AiProvider
{
    Q_OBJECT

public:
    ChatGptProvider(AiStorage *storage, const AiEndpoints &endpoints, BrowserOpener openBrowser,
                    QObject *parent = nullptr);

    QString id() const override { return QStringLiteral("chatgpt"); }
    QString name() const override { return QStringLiteral("ChatGPT"); }

    void signIn() override;
    void cancelSignIn() override;
    void signOut() override;
    void refreshModels() override;
    AiStream *stream(const AiRequest &request) override;

    static constexpr qint64 kRefreshMarginSecs = 5 * 60;
    // Clock skew allowed when checking the ID token's exp.
    static constexpr qint64 kClockSkewSecs = 5;

    // OpenAI's documented URLs: usage settings, and help.
    static QString manageUsageUrl() { return QStringLiteral("https://chatgpt.com/settings/usage"); }

private:
    struct Pending
    {
        QByteArray state, nonce, verifier;
        QString redirectUri;
        QString clientId; // the saved one, or "" for a first registration
    };

    QString hostId();
    void onRedirect(const QUrlQuery &query);
    void exchangeCode(const QString &clientId, const QString &code);
    void verifyAndStore(const QString &clientId, const QJsonObject &tokens);
    void failSignIn(const QString &message);

    QJsonObject loadTokens();
    void saveTokens(const QJsonObject &tokens);
    void forgetTokens();
    // Calls `ready` with a usable access token, refreshing first if needed,
    // or with "" and `error` if there is none (signed out, refresh failed).
    void withAccessToken(std::function<void(const QString &token, const QString &error)> ready);
    void refresh();
    void finishRefresh(const QString &token, const QString &error);

    void onHttpError(AiStream *stream, int status, const QByteArray &body);
    static void failFromCode(AiStream *stream, const QString &code, const QString &fallback);

    QUrl authUrl(const QString &path) const;
    QUrl apiUrl(const QString &path) const;

    OAuthLoopback m_loopback;
    QTimer m_signInTimeout;
    Pending m_pending;
    // Callers waiting on the refresh in flight, if any.
    QList<std::function<void(const QString &, const QString &)>> m_waiting;
    bool m_refreshing = false;
};
