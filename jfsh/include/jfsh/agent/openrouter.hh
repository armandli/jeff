#ifndef JFSH_AGENT_OPENROUTER_HH
#define JFSH_AGENT_OPENROUTER_HH
// jfsh/agent/openrouter.hh — OpenRouter backend.
//
// OpenRouter exposes an OpenAI-compatible API so this is a thin wrapper
// around OpenAIBackend that sets the correct base URL and adds the
// HTTP-Referer / X-Title headers OpenRouter recommends.

#include <jfsh/agent/openai.hh>
#include <jfsh/agent/provider.hh>

namespace jfsh {

struct OpenRouterBackend : AgentBackend {
  explicit OpenRouterBackend(ProviderConfig cfg);

  Result<ChatResponse> chat(const ChatRequest& req) override;
  Result<ChatResponse> chat_stream(
    const ChatRequest& req, StreamCallback cb) override;

private:
  static constexpr std::string_view kBaseUrl =
    "https://openrouter.ai/api/v1";
  static constexpr std::string_view kReferer  =
    "https://github.com/jfsh-shell/jfsh";
  static constexpr std::string_view kTitle = "jfsh";

  OpenAIBackend  mDelegate;
  ProviderConfig mCfg;
};

}  // namespace jfsh

#endif  // JFSH_AGENT_OPENROUTER_HH
