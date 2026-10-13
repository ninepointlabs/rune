#pragma once

// The AI layer's part of --auto-test: SSE parsing, PKCE, JWT verification,
// and both providers' full sign-in / streaming / sign-out flows against a
// local mock server standing in for auth.openai.com, api.openai.com and
// openrouter.ai, with a simulated browser following the redirect. No real
// network, keyring, settings or browser is touched.

#include <functional>

class QQuickWindow;
class QString;

void runAiAutoTest(QQuickWindow *window, const std::function<void(bool, const QString &)> &check);
