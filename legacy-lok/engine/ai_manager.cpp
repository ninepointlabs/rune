#include "ai_manager.h"

AiManager::AiManager()
    : m_providers{
          {"claude", {"https://api.anthropic.com/v1/messages"}},
          {"chatgpt", {"https://api.openai.com/v1/chat/completions"}},
          {"grok", {"https://api.x.ai/v1/chat/completions"}},
      }
{
}

bool AiManager::hasProvider(const std::string &provider) const
{
    return m_providers.count(provider) != 0;
}

bool AiManager::setToken(const std::string &provider, const std::string &token)
{
    if (!hasProvider(provider) || token.empty())
        return false;
    m_tokens[provider] = token;
    return true;
}

bool AiManager::hasToken(const std::string &provider) const
{
    return m_tokens.count(provider) != 0;
}

AiManager::Result AiManager::sendMessage(const std::string &provider, const std::string &model,
                                         const std::string &systemPrompt,
                                         const std::string &userMessage) const
{
    (void)systemPrompt; // optional; unused until requests are real
    Result r;
    if (!hasProvider(provider))
        r.error = "unknown provider: " + provider;
    else if (model.empty())
        r.error = "missing \"model\"";
    else if (userMessage.empty())
        r.error = "missing \"user\"";
    else {
        r.ok = true;
        r.content = "[AI response will go here]";
        r.model = provider;
    }
    return r;
}
