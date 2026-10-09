#include "Jwt.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

#include <memory>

namespace {

QByteArray fromBase64Url(const QByteArray &part, bool *ok)
{
    const auto result = QByteArray::fromBase64Encoding(
        part, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    *ok = bool(result);
    return *result;
}

std::optional<QJsonObject> jsonObject(const QByteArray &bytes)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    return doc.object();
}

template<typename T, void (*Free)(T *)>
using Owned = std::unique_ptr<T, std::integral_constant<decltype(Free), Free>>;

// The RSA public key with modulus `n` and exponent `e` (big-endian bytes).
EVP_PKEY *rsaPublicKey(const QByteArray &n, const QByteArray &e)
{
    Owned<BIGNUM, BN_free> bn(BN_bin2bn(reinterpret_cast<const unsigned char *>(n.constData()), int(n.size()), nullptr));
    Owned<BIGNUM, BN_free> be(BN_bin2bn(reinterpret_cast<const unsigned char *>(e.constData()), int(e.size()), nullptr));
    Owned<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free> build(OSSL_PARAM_BLD_new());
    if (!bn || !be || !build || !OSSL_PARAM_BLD_push_BN(build.get(), OSSL_PKEY_PARAM_RSA_N, bn.get())
        || !OSSL_PARAM_BLD_push_BN(build.get(), OSSL_PKEY_PARAM_RSA_E, be.get()))
        return nullptr;
    Owned<OSSL_PARAM, OSSL_PARAM_free> params(OSSL_PARAM_BLD_to_param(build.get()));
    Owned<EVP_PKEY_CTX, EVP_PKEY_CTX_free> ctx(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr));
    EVP_PKEY *key = nullptr;
    if (!params || !ctx || EVP_PKEY_fromdata_init(ctx.get()) <= 0
        || EVP_PKEY_fromdata(ctx.get(), &key, EVP_PKEY_PUBLIC_KEY, params.get()) <= 0)
        return nullptr;
    return key;
}

} // namespace

namespace Jwt {

std::optional<Token> decode(const QByteArray &jwt)
{
    const QList<QByteArray> parts = jwt.split('.');
    if (parts.size() != 3)
        return std::nullopt;
    bool okHeader = false, okPayload = false, okSignature = false;
    const auto header = jsonObject(fromBase64Url(parts[0], &okHeader));
    const auto payload = jsonObject(fromBase64Url(parts[1], &okPayload));
    const QByteArray signature = fromBase64Url(parts[2], &okSignature);
    if (!okHeader || !okPayload || !okSignature || !header || !payload)
        return std::nullopt;
    return Token{*header, *payload, parts[0] + '.' + parts[1], signature};
}

bool verifyRs256(const Token &token, const QJsonObject &jwks, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (token.header.value(QLatin1String("alg")).toString() != QLatin1String("RS256"))
        return fail(QStringLiteral("unexpected signing algorithm %1").arg(token.header.value(QLatin1String("alg")).toString()));
    const QString kid = token.header.value(QLatin1String("kid")).toString();
    QJsonObject jwk;
    for (const QJsonValue &k : jwks.value(QLatin1String("keys")).toArray()) {
        const QJsonObject candidate = k.toObject();
        if (candidate.value(QLatin1String("kid")).toString() == kid
            && candidate.value(QLatin1String("kty")).toString() == QLatin1String("RSA")) {
            jwk = candidate;
            break;
        }
    }
    if (jwk.isEmpty())
        return fail(QStringLiteral("no published RSA key with kid \"%1\"").arg(kid));
    bool okN = false, okE = false;
    const QByteArray n = fromBase64Url(jwk.value(QLatin1String("n")).toString().toLatin1(), &okN);
    const QByteArray e = fromBase64Url(jwk.value(QLatin1String("e")).toString().toLatin1(), &okE);
    if (!okN || !okE)
        return fail(QStringLiteral("malformed key %1").arg(kid));
    Owned<EVP_PKEY, EVP_PKEY_free> key(rsaPublicKey(n, e));
    Owned<EVP_MD_CTX, EVP_MD_CTX_free> md(EVP_MD_CTX_new());
    if (!key || !md || EVP_DigestVerifyInit(md.get(), nullptr, EVP_sha256(), nullptr, key.get()) <= 0)
        return fail(QStringLiteral("cannot load key %1").arg(kid));
    const int verified = EVP_DigestVerify(
        md.get(), reinterpret_cast<const unsigned char *>(token.signature.constData()), size_t(token.signature.size()),
        reinterpret_cast<const unsigned char *>(token.signingInput.constData()), size_t(token.signingInput.size()));
    if (verified != 1)
        return fail(QStringLiteral("signature does not verify"));
    return true;
}

} // namespace Jwt
