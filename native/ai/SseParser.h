#pragma once

// Incremental parser for a text/event-stream (server-sent events) body:
// feed it bytes as they arrive, in chunks of any size, and it returns each
// complete event. Handles LF and CRLF line ends, multi-line data (joined
// with "\n") and comment lines (": ..."), per the WHATWG EventSource spec.
// Fields other than event and data are ignored.

#include <QByteArray>
#include <QList>
#include <QString>

class SseParser
{
public:
    struct Event
    {
        QString event; // "" when the stream didn't name it
        QByteArray data;
    };

    QList<Event> feed(const QByteArray &bytes);

private:
    void line(const QByteArray &text, QList<Event> &out);

    QByteArray m_buffer;
    QString m_event;
    QByteArray m_data;
    bool m_hasData = false;
};
