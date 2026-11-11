#ifndef JFSH_AGENT_ANTHROPIC_HH
#define JFSH_AGENT_ANTHROPIC_HH
// jfsh/agent/anthropic.hh — Anthropic Messages API backend.
//
// Implements AgentBackend using POST /v1/messages with the x-api-key header.

#include <jfsh/agent/agent.hh>
#include <jfsh/agent/provider.hh>

namespace jfsh {

struct AnthropicBackend : AgentBackend {
  explicit AnthropicBackend(ProviderConfig cfg);

  Result<ChatResponse> chat(const ChatRequest& req) override;
  Result<ChatResponse> chat_stream(
    const ChatRequest& req, StreamCallback cb) override;

protected:
  std::string         build_body(const ChatRequest& req) const;
  Result<ChatResponse> parse_response(std::string_view json) const;
  std::optional<std::string> parse_sse_chunk(std::string_view line) const;

  Result<std::string> http_post(
    std::string_view url,
    std::string_view body) const;

private:
  static constexpr std::string_view kApiVersion = "2023-06-01";

  ProviderConfig mCfg;
};

}  // namespace jfsh

#endif  // JFSH_AGENT_ANTHROPIC_HH
