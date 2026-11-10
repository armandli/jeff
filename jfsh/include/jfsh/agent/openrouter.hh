#pragma once
// jfsh/agent/openrouter.hh — OpenRouter backend.
//
// OpenRouter (openrouter.ai) exposes an OpenAI-compatible API so this is a
// thin wrapper around OpenAIBackend that sets the correct base URL and
// adds the HTTP-Referer / X-Title headers OpenRouter recommends.

#include <jfsh/agent/openai.hh>
#include <jfsh/agent/provider.hh>

namespace jfsh {

class OpenRouterBackend : public AgentBackend {
public:
    explicit OpenRouterBackend(ProviderConfig cfg);

    Result<ChatResponse> chat(const ChatRequest& req) override;
    Result<ChatResponse> chat_stream(const ChatRequest& req,
                                     StreamCallback cb) override;

private:
    // Delegates to OpenAIBackend after patching the base URL and headers.
    OpenAIBackend delegate_;
    ProviderConfig cfg_;

    static constexpr std::string_view kBaseUrl = "https://openrouter.ai/api/v1";
    static constexpr std::string_view kReferer  = "https://github.com/jfsh-shell/jfsh";
    static constexpr std::string_view kTitle    = "jfsh";
};

} // namespace jfsh
