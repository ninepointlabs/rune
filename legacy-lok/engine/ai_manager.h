// AI provider registry and token store. Skeleton: no network calls yet.
//
// Tokens live in memory only; they will move to the system keychain once
// OAuth lands. sendMessage() validates its inputs and returns a placeholder.

#pragma once

#include <map>
#include <string>

class AiManager {
public:
    struct Provider {
        std::string apiUrl;
    };
    struct Result {
        bool ok = false;
        std::string error;   // set when !ok
        std::string content; // set when ok
        std::string model;
    };

    AiManager();

    const std::map<std::string, Provider> &providers() const { return m_providers; }
    bool hasProvider(const std::string &provider) const;

    // False if the provider is unknown or the token is empty.
    bool setToken(const std::string &provider, const std::string &token);
    bool hasToken(const std::string &provider) const;

    Result sendMessage(const std::string &provider, const std::string &model,
                       const std::string &systemPrompt, const std::string &userMessage) const;

private:
    std::map<std::string, Provider> m_providers;
    std::map<std::string, std::string> m_tokens;
};
