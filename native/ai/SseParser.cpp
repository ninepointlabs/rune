#include "SseParser.h"

QList<SseParser::Event> SseParser::feed(const QByteArray &bytes)
{
    QList<Event> out;
    m_buffer += bytes;
    for (;;) {
        const qsizetype newline = m_buffer.indexOf('\n');
        if (newline < 0)
            break;
        QByteArray text = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (text.endsWith('\r'))
            text.chop(1);
        line(text, out);
    }
    return out;
}

void SseParser::line(const QByteArray &text, QList<Event> &out)
{
    if (text.isEmpty()) { // blank line: dispatch
        if (m_hasData)
            out.append({m_event, m_data});
        m_event.clear();
        m_data.clear();
        m_hasData = false;
        return;
    }
    if (text.startsWith(':'))
        return; // comment / keep-alive
    const qsizetype colon = text.indexOf(':');
    const QByteArray field = colon < 0 ? text : text.left(colon);
    QByteArray value = colon < 0 ? QByteArray() : text.mid(colon + 1);
    if (value.startsWith(' '))
        value.remove(0, 1);
    if (field == "event") {
        m_event = QString::fromUtf8(value);
    } else if (field == "data") {
        if (m_hasData)
            m_data += '\n';
        m_data += value;
        m_hasData = true;
    }
}
