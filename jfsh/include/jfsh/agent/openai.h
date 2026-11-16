#ifndef JFSH_AGENT_OPENAI_H
#define JFSH_AGENT_OPENAI_H
// jfsh/agent/openai.h — OpenAI chat completion backend.
//
// Implements AgentBackend using the OpenAI REST API (POST /v1/chat/completions).
// Also compatible with OpenRouter (which uses the same API shape).

#include <jfsh/agent/agent.h>
#include <jfsh/agent/provider.h>
#include <jfsh/core/aliases.h>

namespace jfsh {

struct OpenAIBackend : AgentBackend {
  explicit OpenAIBackend(ProviderConfig cfg);

  Result<ChatResponse> chat(const ChatRequest& req) override;
  Result<ChatResponse> chat_stream(
    const ChatRequest& req, StreamCallback cb) override;

protected:
  str         build_body(const ChatRequest& req) const;
  Result<ChatResponse> parse_response(strv json) const;
  opt<str> parse_sse_chunk(strv line) const;

  Result<str> http_post(
    strv url,
    strv body,
    strv extra_headers = {}) const;

private:
  ProviderConfig mCfg;
};

}  // namespace jfsh

#endif  // JFSH_AGENT_OPENAI_H
