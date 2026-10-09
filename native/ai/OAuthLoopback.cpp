#include "OAuthLoopback.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

namespace OAuth {

QByteArray base64Url(const QByteArray &bytes)
{
    return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QByteArray randomToken(int bytes)
{
    QByteArray buffer(bytes, Qt::Uninitialized);
    // QRandomGenerator::system() is the OS CSPRNG (getrandom()).
    QRandomGenerator::system()->generate(buffer.begin(), buffer.end());
    return base64Url(buffer);
}

QByteArray pkceChallenge(const QByteArray &verifier)
{
    return base64Url(QCryptographicHash::hash(verifier, QCryptographicHash::Sha256));
}

QByteArray formEncode(const QList<std::pair<QString, QString>> &fields)
{
    QByteArray out;
    for (const auto &[name, value] : fields) {
        if (!out.isEmpty())
            out += '&';
        out += QUrl::toPercentEncoding(name) + '=' + QUrl::toPercentEncoding(value);
    }
    return out;
}

} // namespace OAuth

namespace {

// Requests larger than this are not an OAuth redirect.
constexpr qsizetype kMaxRequest = 16 * 1024;

const QByteArray kDonePage =
    "<!doctype html><meta charset=utf-8><title>Rune</title>"
    "<body style=\"font-family:sans-serif;background:#1a1b26;color:#c0caf5;display:flex;"
    "align-items:center;justify-content:center;height:90vh\">"
    "<p>Sign-in finished. You can close this tab and return to Rune.</p>";

} // namespace

OAuthLoopback::OAuthLoopback(QObject *parent) : QObject(parent) {}

OAuthLoopback::~OAuthLoopback()
{
    close();
}

bool OAuthLoopback::listen(const QString &host, const QString &path)
{
    close();
    m_done = false;
    m_path = path;

    auto *v4 = new QTcpServer(this);
    if (!v4->listen(QHostAddress::LocalHost, 0)) {
        delete v4;
        return false;
    }
    m_servers.append(v4);
    const quint16 port = v4->serverPort();
    if (host == QLatin1String("localhost")) {
        auto *v6 = new QTcpServer(this);
        if (v6->listen(QHostAddress::LocalHostIPv6, port))
            m_servers.append(v6);
        else
            delete v6; // no IPv6 loopback here; IPv4 alone will do
    }
    for (QTcpServer *server : std::as_const(m_servers))
        connect(server, &QTcpServer::newConnection, this, [this, server] { onConnection(server); });
    m_redirectUri = QStringLiteral("http://%1:%2%3").arg(host).arg(port).arg(path);
    return true;
}

void OAuthLoopback::close()
{
    for (QTcpServer *server : std::as_const(m_servers)) {
        server->close();
        server->deleteLater();
    }
    m_servers.clear();
}

void OAuthLoopback::onConnection(QTcpServer *server)
{
    while (QTcpSocket *socket = server->nextPendingConnection()) {
        if (!socket->peerAddress().isLoopback()) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        // Not the server's child: close() deletes the servers, and the
        // response must still reach the browser.
        socket->setParent(this);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void OAuthLoopback::onReadyRead(QTcpSocket *socket)
{
    QByteArray request = socket->property("request").toByteArray() + socket->readAll();
    if (request.size() > kMaxRequest) {
        respond(socket, 400, "Bad request");
        return;
    }
    if (!request.contains("\r\n\r\n")) {
        socket->setProperty("request", request);
        return; // headers not complete yet
    }
    // "GET /path?query HTTP/1.1"
    const QList<QByteArray> requestLine = request.left(request.indexOf("\r\n")).split(' ');
    if (requestLine.size() < 2 || requestLine[0] != "GET") {
        respond(socket, 405, "Method not allowed");
        return;
    }
    const QUrl target(QString::fromLatin1(requestLine[1]));
    if (m_done || target.path() != m_path) {
        respond(socket, 404, "Not found");
        return;
    }
    m_done = true;
    respond(socket, 200, kDonePage);
    close();
    emit received(QUrlQuery(target));
}

void OAuthLoopback::respond(QTcpSocket *socket, int status, const QByteArray &html)
{
    const QByteArray reason = status == 200 ? "OK" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed"
                                                                                                   : "Bad Request";
    socket->write("HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n"
                  "Content-Type: text/html; charset=utf-8\r\n"
                  "Cache-Control: no-store\r\n"
                  "Connection: close\r\n"
                  "Content-Length: " + QByteArray::number(html.size()) + "\r\n\r\n" + html);
    socket->disconnectFromHost();
}
