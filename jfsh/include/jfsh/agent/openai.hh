#pragma once
// jfsh/agent/openai.hh — OpenAI chat completion backend.
//
// Implements AgentBackend using the OpenAI REST API (POST /v1/chat/completions).
// Also compatible with OpenRouter (which uses the same API shape).

#include <jfsh/agent/agent.hh>
#include <jfsh/agent/provider.hh>

namespace jfsh {

class OpenAIBackend : public AgentBackend {
public:
    explicit OpenAIBackend(ProviderConfig cfg);

    Result<ChatResponse> chat(const ChatRequest& req) override;
    Result<ChatResponse> chat_stream(const ChatRequest& req,
                                     StreamCallback cb) override;

private:
    ProviderConfig cfg_;

    // Build the JSON request body.
    std::string build_body(const ChatRequest& req) const;

    // Parse a non-streaming response JSON.
    Result<ChatResponse> parse_response(std::string_view json) const;

    // Parse one SSE data line from a streaming response.
    std::optional<std::string> parse_sse_chunk(std::string_view line) const;

    // HTTP POST with the Authorization: Bearer header.
    Result<std::string> http_post(std::string_view url,
                                  std::string_view body,
                                  std::string_view extra_headers = {}) const;
};

} // namespace jfsh
