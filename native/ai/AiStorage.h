#pragma once

// Where the AI layer keeps sign-in state. Secrets (tokens, API keys) and
// plain settings (which account, the chosen model) are kept apart:
// SystemAiStorage puts secrets in the desktop keyring (Secret Service, via
// libsecret) and settings in QSettings; MemoryAiStorage keeps both in
// memory, for tests, so they never touch the user's real keyring or
// settings.

#include <QHash>
#include <QSettings>
#include <QString>
#include <QVariant>

class AiStorage
{
public:
    virtual ~AiStorage() = default;

    // "" if absent or unreadable (no Secret Service, or the keyring stayed
    // locked). May block briefly, or while the keyring asks to be unlocked.
    virtual QString secret(const QString &key) = 0;
    // False if it couldn't be stored.
    virtual bool setSecret(const QString &key, const QString &value) = 0;
    virtual void removeSecret(const QString &key) = 0;

    virtual QVariant value(const QString &key) const = 0;
    virtual void setValue(const QString &key, const QVariant &value) = 0;
    virtual void removeValue(const QString &key) = 0;
};

class SystemAiStorage : public AiStorage
{
public:
    QString secret(const QString &key) override;
    bool setSecret(const QString &key, const QString &value) override;
    void removeSecret(const QString &key) override;

    QVariant value(const QString &key) const override { return m_settings.value(key); }
    void setValue(const QString &key, const QVariant &value) override { m_settings.setValue(key, value); }
    void removeValue(const QString &key) override { m_settings.remove(key); }

private:
    QSettings m_settings; // ~/.config/Rune/Rune.conf
};

class MemoryAiStorage : public AiStorage
{
public:
    QString secret(const QString &key) override { return m_secrets.value(key); }
    bool setSecret(const QString &key, const QString &value) override
    {
        m_secrets.insert(key, value);
        return true;
    }
    void removeSecret(const QString &key) override { m_secrets.remove(key); }

    QVariant value(const QString &key) const override { return m_values.value(key); }
    void setValue(const QString &key, const QVariant &value) override { m_values.insert(key, value); }
    void removeValue(const QString &key) override { m_values.remove(key); }

    QHash<QString, QString> m_secrets;
    QHash<QString, QVariant> m_values;
};
