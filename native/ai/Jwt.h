#pragma once

// Just enough JWT for OpenID Connect ID tokens: decode the three parts and
// verify an RS256 signature against a JWKS document (the issuer's published
// keys). Claim checks (iss, aud, exp, nonce) are the caller's.

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace Jwt {

struct Token
{
    QJsonObject header;
    QJsonObject payload;
    QByteArray signingInput; // "<header>.<payload>", as signed
    QByteArray signature;
};

// nullopt unless `jwt` has three base64url parts and JSON-object header and payload.
std::optional<Token> decode(const QByteArray &jwt);

// True if `token` is RS256 (header alg) and signed by the RSA key in `jwks`
// whose kid matches the header's. Otherwise false, with `error` saying why.
bool verifyRs256(const Token &token, const QJsonObject &jwks, QString *error);

} // namespace Jwt
