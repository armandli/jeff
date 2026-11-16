#ifndef JFSH_AGENT_ANTHROPIC_H
#define JFSH_AGENT_ANTHROPIC_H
// jfsh/agent/anthropic.h — Anthropic Messages API backend.
//
// Implements AgentBackend using POST /v1/messages with the x-api-key header.

#include <jfsh/agent/agent.h>
#include <jfsh/agent/provider.h>
#include <jfsh/core/aliases.h>

namespace jfsh {

struct AnthropicBackend : AgentBackend {
  explicit AnthropicBackend(ProviderConfig cfg);

  Result<ChatResponse> chat(const ChatRequest& req) override;
  Result<ChatResponse> chat_stream(
    const ChatRequest& req, StreamCallback cb) override;

protected:
  str         build_body(const ChatRequest& req) const;
  Result<ChatResponse> parse_response(strv json) const;
  opt<str> parse_sse_chunk(strv line) const;

  Result<str> http_post(
    strv url,
    strv body) const;

private:
  static constexpr strv kApiVersion = "2023-06-01";

  ProviderConfig mCfg;
};

}  // namespace jfsh

#endif  // JFSH_AGENT_ANTHROPIC_H
