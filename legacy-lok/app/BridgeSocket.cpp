#include "BridgeSocket.h"

#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QtLogging>

BridgeSocket::BridgeSocket(QObject *parent)
    : QObject(parent)
    , m_socketPath(defaultSocketPath())
{
    m_retry.setSingleShot(true);
    m_retry.setInterval(1000);
    connect(&m_retry, &QTimer::timeout, this, &BridgeSocket::connectToEngine);

    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        setErrorString({});
        emit connectedChanged();
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, &BridgeSocket::onDisconnected);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, &BridgeSocket::onSocketError);
    connect(&m_socket, &QLocalSocket::readyRead, this, &BridgeSocket::onReadyRead);
    // Covers both failed connects and dropped connections. errorOccurred
    // fires before the state settles, so retry from here rather than there.
    connect(&m_socket, &QLocalSocket::stateChanged, this, [this](QLocalSocket::LocalSocketState state) {
        if (state == QLocalSocket::UnconnectedState && m_wanted && m_autoReconnect)
            m_retry.start();
    });
}

QString BridgeSocket::defaultSocketPath()
{
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return runtime.isEmpty() ? QStringLiteral("/tmp/rune-engine.sock")
                             : runtime + QStringLiteral("/rune-engine.sock");
}

void BridgeSocket::setSocketPath(const QString &path)
{
    if (path == m_socketPath)
        return;
    m_socketPath = path;
    emit socketPathChanged();
}

bool BridgeSocket::isConnected() const
{
    return m_socket.state() == QLocalSocket::ConnectedState;
}

void BridgeSocket::setRetryInterval(int ms)
{
    if (ms == m_retry.interval())
        return;
    m_retry.setInterval(ms);
    emit retryIntervalChanged();
}

void BridgeSocket::setOnEvent(const QJSValue &callback)
{
    if (callback.strictlyEquals(m_onEvent))
        return;
    m_onEvent = callback;
    emit onEventChanged();
}

void BridgeSocket::connectToEngine()
{
    m_wanted = true;
    if (m_socket.state() != QLocalSocket::UnconnectedState)
        return;
    m_inbuf.clear();
    m_socket.connectToServer(m_socketPath);
}

void BridgeSocket::disconnectFromEngine()
{
    m_wanted = false;
    m_retry.stop();
    m_socket.abort();
}

int BridgeSocket::send(const QVariantMap &command, const QJSValue &callback)
{
    if (!isConnected()) {
        // Defer so callers see the same async contract as a real reply.
        QMetaObject::invokeMethod(this, [this, callback] {
            invoke(callback, {{"ok", false}, {"error", QStringLiteral("not connected to engine")}});
        }, Qt::QueuedConnection);
        return -1;
    }

    const int id = m_nextId++;
    QVariantMap msg = command;
    msg.insert(QStringLiteral("id"), id);
    QByteArray line = QJsonDocument(QJsonObject::fromVariantMap(msg)).toJson(QJsonDocument::Compact);
    line.append('\n');

    if (callback.isCallable())
        m_pending.insert(id, callback);
    m_socket.write(line);
    return id;
}

void BridgeSocket::onReadyRead()
{
    m_inbuf.append(m_socket.readAll());
    qsizetype nl;
    while ((nl = m_inbuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_inbuf.left(nl).trimmed();
        m_inbuf.remove(0, nl + 1);
        if (line.isEmpty())
            continue;

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (!doc.isObject()) {
            qWarning("BridgeSocket: bad reply from engine: %s", qPrintable(err.errorString()));
            continue;
        }
        const QVariantMap reply = doc.object().toVariantMap();
        emit responseReceived(reply);

        if (!reply.contains(QStringLiteral("id"))) {
            invoke(m_onEvent, reply);
            continue;
        }
        bool hasId = false;
        const int id = reply.value(QStringLiteral("id")).toInt(&hasId);
        if (hasId) {
            if (QJSValue cb = m_pending.take(id); cb.isCallable())
                invoke(cb, reply);
        }
    }
}

void BridgeSocket::onDisconnected()
{
    failPending(QStringLiteral("engine disconnected"));
    if (m_errorString.isEmpty())
        setErrorString(QStringLiteral("Engine disconnected"));
    emit connectedChanged();
}

void BridgeSocket::onSocketError(QLocalSocket::LocalSocketError error)
{
    switch (error) {
    case QLocalSocket::ServerNotFoundError:
    case QLocalSocket::ConnectionRefusedError:
        setErrorString(QStringLiteral("Engine not running at %1").arg(m_socketPath));
        break;
    case QLocalSocket::PeerClosedError:
        setErrorString(QStringLiteral("Engine closed the connection"));
        break;
    default:
        setErrorString(m_socket.errorString());
        break;
    }
}

void BridgeSocket::setErrorString(const QString &message)
{
    if (message == m_errorString)
        return;
    m_errorString = message;
    emit errorStringChanged();
}

void BridgeSocket::failPending(const QString &message)
{
    const auto pending = std::exchange(m_pending, {});
    for (const QJSValue &cb : pending)
        invoke(cb, {{"ok", false}, {"error", message}});
}

void BridgeSocket::invoke(QJSValue callback, const QVariantMap &response)
{
    if (!callback.isCallable())
        return;
    QJSEngine *engine = qjsEngine(this);
    if (!engine)
        return;
    const QJSValue result = callback.call({engine->toScriptValue(response)});
    if (result.isError())
        qWarning("BridgeSocket: callback threw: %s", qPrintable(result.toString()));
}
