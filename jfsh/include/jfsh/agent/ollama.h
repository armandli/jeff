#ifndef JFSH_AGENT_OLLAMA_H
#define JFSH_AGENT_OLLAMA_H
// jfsh/agent/ollama.h — Ollama local LLM backend.
//
// Implements AgentBackend using Ollama's REST API (POST /api/chat).
// Default base URL: http://localhost:11434.

#include <jfsh/agent/agent.h>
#include <jfsh/agent/provider.h>
#include <jfsh/core/aliases.h>

namespace jfsh {

struct OllamaBackend : AgentBackend {
  explicit OllamaBackend(ProviderConfig cfg);

  Result<ChatResponse> chat(const ChatRequest& req) override;
  Result<ChatResponse> chat_stream(
    const ChatRequest& req, StreamCallback cb) override;

  bool is_server_available() const noexcept;

protected:
  str         build_body(const ChatRequest& req, bool stream) const;
  Result<ChatResponse> parse_response(strv json) const;
  // Ollama streams NDJSON (one JSON object per line, not SSE).
  opt<str> parse_ndjson_chunk(strv line) const;

  Result<str> http_post(
    strv url,
    strv body) const;

private:
  ProviderConfig mCfg;
};

}  // namespace jfsh

#endif  // JFSH_AGENT_OLLAMA_H
