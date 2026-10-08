#pragma once

// BridgeSocket: QML-facing client for the rune-engine JSON-lines protocol.
//
//   BridgeSocket {
//       id: bridge
//       socketPath: "/run/user/1000/rune-engine.sock"
//       onConnectedChanged: if (connected) send({cmd: "ping"}, r => console.log(r.ok))
//   }
//
// send() assigns the request id and invokes the callback with the parsed
// reply. If the engine is unreachable or drops the connection, pending
// callbacks receive {ok: false, error: "..."}.

#include <QByteArray>
#include <QHash>
#include <QJSValue>
#include <QLocalSocket>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantMap>

class BridgeSocket : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString socketPath READ socketPath WRITE setSocketPath NOTIFY socketPathChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    // Keep retrying (every retryInterval ms) while the engine is not up.
    Q_PROPERTY(bool autoReconnect MEMBER m_autoReconnect NOTIFY autoReconnectChanged)
    Q_PROPERTY(int retryInterval READ retryInterval WRITE setRetryInterval NOTIFY retryIntervalChanged)

public:
    explicit BridgeSocket(QObject *parent = nullptr);

    static QString defaultSocketPath();

    QString socketPath() const { return m_socketPath; }
    void setSocketPath(const QString &path);
    bool isConnected() const;
    QString errorString() const { return m_errorString; }
    int retryInterval() const { return m_retry.interval(); }
    void setRetryInterval(int ms);

    Q_INVOKABLE void connectToEngine();
    Q_INVOKABLE void disconnectFromEngine();
    // Returns the request id, or -1 if not connected (callback still fires).
    Q_INVOKABLE int send(const QVariantMap &command, const QJSValue &callback = QJSValue());

signals:
    void socketPathChanged();
    void connectedChanged();
    void errorStringChanged();
    void autoReconnectChanged();
    void retryIntervalChanged();
    // Every reply, including ones that also went to a callback.
    void responseReceived(const QVariantMap &response);

private:
    void onReadyRead();
    void onDisconnected();
    void onSocketError(QLocalSocket::LocalSocketError error);
    void setErrorString(const QString &message);
    void failPending(const QString &message);
    void invoke(QJSValue callback, const QVariantMap &response);

    QLocalSocket m_socket;
    QTimer m_retry;
    QString m_socketPath;
    QString m_errorString;
    QByteArray m_inbuf;
    QHash<int, QJSValue> m_pending;
    int m_nextId = 1;
    bool m_autoReconnect = true;
    bool m_wanted = false; // connectToEngine() called and not cancelled
};
