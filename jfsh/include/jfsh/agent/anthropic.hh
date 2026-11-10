#pragma once
// jfsh/agent/anthropic.hh — Anthropic Messages API backend.
//
// Implements AgentBackend using POST /v1/messages with the x-api-key header.
// Handles Anthropic's slightly different message format (no system inside
// messages array; separate top-level "system" field).

#include <jfsh/agent/agent.hh>
#include <jfsh/agent/provider.hh>

namespace jfsh {

class AnthropicBackend : public AgentBackend {
public:
    explicit AnthropicBackend(ProviderConfig cfg);

    Result<ChatResponse> chat(const ChatRequest& req) override;
    Result<ChatResponse> chat_stream(const ChatRequest& req,
                                     StreamCallback cb) override;

private:
    ProviderConfig cfg_;

    // Anthropic API version header value.
    static constexpr std::string_view kApiVersion = "2023-06-01";

    std::string build_body(const ChatRequest& req) const;
    Result<ChatResponse> parse_response(std::string_view json) const;
    std::optional<std::string> parse_sse_chunk(std::string_view line) const;

    Result<std::string> http_post(std::string_view url,
                                  std::string_view body) const;
};

} // namespace jfsh
