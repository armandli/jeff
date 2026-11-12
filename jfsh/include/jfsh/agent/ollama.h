#ifndef JFSH_AGENT_OLLAMA_H
#define JFSH_AGENT_OLLAMA_H
// jfsh/agent/ollama.h — Ollama local LLM backend.
//
// Implements AgentBackend using Ollama's REST API (POST /api/chat).
// Default base URL: http://localhost:11434.

#include <jfsh/agent/agent.h>
#include <jfsh/agent/provider.h>

namespace jfsh {

struct OllamaBackend : AgentBackend {
  explicit OllamaBackend(ProviderConfig cfg);

  Result<ChatResponse> chat(const ChatRequest& req) override;
  Result<ChatResponse> chat_stream(
    const ChatRequest& req, StreamCallback cb) override;

  bool is_server_available() const noexcept;

protected:
  std::string         build_body(const ChatRequest& req, bool stream) const;
  Result<ChatResponse> parse_response(std::string_view json) const;
  // Ollama streams NDJSON (one JSON object per line, not SSE).
  std::optional<std::string> parse_ndjson_chunk(std::string_view line) const;

  Result<std::string> http_post(
    std::string_view url,
    std::string_view body) const;

private:
  ProviderConfig mCfg;
};

}  // namespace jfsh

#endif  // JFSH_AGENT_OLLAMA_H
