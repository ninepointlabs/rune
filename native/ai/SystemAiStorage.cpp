// libsecret before any Qt header: GLib's headers use `signals` as an
// identifier, which Qt defines as a macro.
#include <libsecret/secret.h>

#include "AiStorage.h"

#include <QDebug>

namespace {

// Items are found by the "key" attribute; the schema name keeps them apart
// from other applications' secrets.
const SecretSchema *schema()
{
    static const SecretSchema s = {
        "com.ninepointlabs.Rune", SECRET_SCHEMA_NONE,
        {{"key", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    return &s;
}

void warn(const char *what, const QString &key, GError *error)
{
    qWarning().noquote() << "SystemAiStorage:" << what << key << "-" << (error ? error->message : "unknown error");
    if (error)
        g_error_free(error);
}

} // namespace

QString SystemAiStorage::secret(const QString &key)
{
    GError *error = nullptr;
    const QByteArray k = key.toUtf8();
    gchar *value = secret_password_lookup_sync(schema(), nullptr, &error, "key", k.constData(), nullptr);
    if (error) {
        warn("cannot read", key, error);
        return {};
    }
    const QString result = value ? QString::fromUtf8(value) : QString();
    secret_password_free(value);
    return result;
}

bool SystemAiStorage::setSecret(const QString &key, const QString &value)
{
    GError *error = nullptr;
    const QByteArray k = key.toUtf8(), v = value.toUtf8();
    const QByteArray label = "Rune: " + k;
    const bool stored = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, label.constData(),
                                                   v.constData(), nullptr, &error, "key", k.constData(), nullptr);
    if (error || !stored) {
        warn("cannot store", key, error);
        return false;
    }
    return true;
}

void SystemAiStorage::removeSecret(const QString &key)
{
    GError *error = nullptr;
    const QByteArray k = key.toUtf8();
    secret_password_clear_sync(schema(), nullptr, &error, "key", k.constData(), nullptr);
    if (error)
        warn("cannot remove", key, error);
}
