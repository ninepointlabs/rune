#pragma once

// The receiving end of a desktop OAuth sign-in: a one-shot HTTP listener on
// the loopback interface that the browser is redirected back to. It accepts
// only connections on loopback addresses, answers the first request for
// `path` with a short "return to Rune" page, reports that request's query
// and stops listening. Requests for other paths (e.g. /favicon.ico) get a
// 404 and are otherwise ignored.
//
// Plus the PKCE (RFC 7636, S256) and random-token helpers both sign-in
// flows need.

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrlQuery>

#include <utility>

class QTcpServer;
class QTcpSocket;

namespace OAuth {
// URL-safe base64 without padding (RFC 4648 §5), as PKCE and JWTs use.
QByteArray base64Url(const QByteArray &bytes);
// `bytes` cryptographically random bytes, base64url-encoded.
QByteArray randomToken(int bytes = 32);
// S256 code_challenge for `verifier`: base64url(SHA-256(verifier)).
QByteArray pkceChallenge(const QByteArray &verifier);
// application/x-www-form-urlencoded, also used for URL queries: every name
// and value fully percent-encoded, so "+", "/", ":" etc. can't be misread.
QByteArray formEncode(const QList<std::pair<QString, QString>> &fields);
} // namespace OAuth

class OAuthLoopback : public QObject
{
    Q_OBJECT

public:
    explicit OAuthLoopback(QObject *parent = nullptr);
    ~OAuthLoopback() override;

    // Starts listening on an OS-chosen port. `host` is what the redirect URI
    // names: "127.0.0.1" listens on IPv4 loopback only; "localhost" on IPv4
    // and, if available, IPv6 loopback on the same port, since browsers may
    // resolve localhost to either. False if no port could be bound.
    bool listen(const QString &host, const QString &path);
    // e.g. "http://127.0.0.1:41234/auth/callback"; "" until listen() succeeds.
    QString redirectUri() const { return m_redirectUri; }
    void close();

signals:
    // The redirect arrived (once per listen()). The listener has stopped.
    void received(const QUrlQuery &query);

private:
    void onConnection(QTcpServer *server);
    void onReadyRead(QTcpSocket *socket);
    static void respond(QTcpSocket *socket, int status, const QByteArray &html);

    QList<QTcpServer *> m_servers;
    QString m_path;
    QString m_redirectUri;
    bool m_done = false;
};
