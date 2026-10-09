#pragma once

// OpenRouter (openrouter.ai), signed in with its OAuth PKCE flow
// (openrouter.ai/docs/guides/overview/auth/oauth, as of 2026-10-09): the
// user approves Rune in the browser and OpenRouter issues an API key that
// the user controls (and can delete from their OpenRouter account). One
// sign-in reaches Claude, GPT and Grok models, billed to the user's
// OpenRouter credits.
//
// - Sign-in: <openrouter>/auth?callback_url=http://localhost:<port>/callback
//   with code_challenge (S256) and state; the code is exchanged at
//   POST /api/v1/auth/keys for {"key": ...}.
// - Models: GET /api/v1/models, limited to the Claude, GPT and Grok
//   families (anthropic/, openai/, x-ai/), without ":variant" ids.
// - Requests: POST /api/v1/chat/completions with stream:true; text from
//   choices[0].delta.content, done at "data: [DONE]".
// - Sign-out forgets the key here. OpenRouter documents no revocation for
//   it, so the user is pointed at the key's page to delete it.
//
// Storage: secret "openrouter/key"; value openrouter/signedIn.

#include "AiProvider.h"
#include "OAuthLoopback.h"

#include <QTimer>

class OpenRouterProvider : public AiProvider
{
    Q_OBJECT

public:
    OpenRouterProvider(AiStorage *storage, const AiEndpoints &endpoints, BrowserOpener openBrowser,
                       QObject *parent = nullptr);

    QString id() const override { return QStringLiteral("openrouter"); }
    QString name() const override { return QStringLiteral("OpenRouter"); }

    void signIn() override;
    void cancelSignIn() override;
    void signOut() override;
    void refreshModels() override;
    // A Claude Sonnet model if listed, else the first.
    QString defaultModel() const override;
    AiStream *stream(const AiRequest &request) override;

    static QStringList modelFamilies() { return {QStringLiteral("anthropic/"), QStringLiteral("openai/"), QStringLiteral("x-ai/")}; }
    static QString creditsUrl() { return QStringLiteral("https://openrouter.ai/settings/credits"); }

private:
    void onRedirect(const QUrlQuery &query);
    void failSignIn(const QString &message);
    QUrl url(const QString &path) const;
    void onHttpError(AiStream *stream, int status, const QByteArray &body);

    OAuthLoopback m_loopback;
    QTimer m_signInTimeout;
    QByteArray m_state, m_verifier;
};
