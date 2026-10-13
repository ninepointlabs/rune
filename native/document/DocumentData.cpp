#include "DocumentData.h"

#include <QRegularExpression>

bool DocumentData::isValidName(const QString &name)
{
    static const QRegularExpression valid(QStringLiteral("^[a-z0-9-]{1,64}$"));
    return valid.match(name).hasMatch();
}

bool DocumentData::setValue(const QString &name, const QJsonValue &value)
{
    if (!isValidName(name))
        return false;
    if (value.isUndefined() || value.isNull()) {
        if (!m_stores.remove(name))
            return true;
    } else if (value.isObject() || value.isArray()) {
        if (m_stores.value(name) == value)
            return true;
        m_stores.insert(name, value);
    } else {
        return false;
    }
    emit changed(name);
    setModified(true);
    return true;
}

void DocumentData::replace(const Stores &stores)
{
    const QStringList before = names();
    m_stores.clear();
    for (auto it = stores.cbegin(); it != stores.cend(); ++it)
        if (isValidName(it.key()) && (it.value().isObject() || it.value().isArray()))
            m_stores.insert(it.key(), it.value());
    for (const QString &name : before)
        if (!m_stores.contains(name))
            emit changed(name);
    for (auto it = m_stores.cbegin(); it != m_stores.cend(); ++it)
        emit changed(it.key());
    setModified(false);
}

void DocumentData::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    emit modificationChanged();
}
