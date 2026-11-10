#ifndef JFSH_AGENT_OPENAI_HH
#define JFSH_AGENT_OPENAI_HH
// jfsh/agent/openai.hh — OpenAI chat completion backend.
//
// Implements AgentBackend using the OpenAI REST API (POST /v1/chat/completions).
// Also compatible with OpenRouter (which uses the same API shape).

#include <jfsh/agent/agent.hh>
#include <jfsh/agent/provider.hh>

namespace jfsh {

struct OpenAIBackend : AgentBackend {
  explicit OpenAIBackend(ProviderConfig cfg);

  Result<ChatResponse> chat(const ChatRequest& req) override;
  Result<ChatResponse> chat_stream(
    const ChatRequest& req, StreamCallback cb) override;

protected:
  std::string         build_body(const ChatRequest& req) const;
  Result<ChatResponse> parse_response(std::string_view json) const;
  std::optional<std::string> parse_sse_chunk(std::string_view line) const;

  Result<std::string> http_post(
    std::string_view url,
    std::string_view body,
    std::string_view extra_headers = {}) const;

private:
  ProviderConfig mCfg;
};

}  // namespace jfsh

#endif  // JFSH_AGENT_OPENAI_HH
