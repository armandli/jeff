#pragma once
// jfsh/agent/ollama.hh — Ollama local LLM backend.
//
// Implements AgentBackend using Ollama's REST API (POST /api/chat).
// Ollama uses a slightly different JSON schema than OpenAI but has no auth.
// Default base URL: http://localhost:11434.

#include <jfsh/agent/agent.hh>
#include <jfsh/agent/provider.hh>

namespace jfsh {

class OllamaBackend : public AgentBackend {
public:
    explicit OllamaBackend(ProviderConfig cfg);

    Result<ChatResponse> chat(const ChatRequest& req) override;
    Result<ChatResponse> chat_stream(const ChatRequest& req,
                                     StreamCallback cb) override;

    // Check whether the Ollama server is reachable.
    bool is_server_available() const noexcept;

private:
    ProviderConfig cfg_;

    std::string build_body(const ChatRequest& req, bool stream) const;
    Result<ChatResponse> parse_response(std::string_view json) const;
    // Ollama streams NDJSON (one JSON object per line, not SSE).
    std::optional<std::string> parse_ndjson_chunk(std::string_view line) const;

    Result<std::string> http_post(std::string_view url,
                                  std::string_view body) const;
};

} // namespace jfsh
