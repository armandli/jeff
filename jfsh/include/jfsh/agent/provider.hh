#pragma once
// jfsh/agent/provider.hh — LLM provider configuration.
//
// Defines the ProviderConfig union and the ProviderKind enum used by all
// agent backends.  The user configures a provider via JFSH_PROVIDER,
// JFSH_API_KEY, JFSH_MODEL, etc. environment variables or ~/.jfshrc.

#include <jfsh/core/types.hh>
#include <optional>
#include <string>
#include <variant>

namespace jfsh {

// ---------------------------------------------------------------------------
// ProviderKind — which LLM backend to use
// ---------------------------------------------------------------------------
enum class ProviderKind : std::uint8_t {
    OpenAI,       // api.openai.com  (Bearer token)
    Anthropic,    // api.anthropic.com  (x-api-key header)
    Ollama,       // local HTTP server (no auth)
    OpenRouter,   // openrouter.ai  (OpenAI-compatible, Bearer token)
};

// ---------------------------------------------------------------------------
// ModelParams — sampling parameters
// ---------------------------------------------------------------------------
struct ModelParams {
    float temperature   = 0.7f;
    float top_p         = 1.0f;
    int   max_tokens    = 4096;
    std::optional<int> seed;
};

// ---------------------------------------------------------------------------
// ProviderConfig — connection settings for one LLM provider
// ---------------------------------------------------------------------------
struct ProviderConfig {
    ProviderKind  kind      = ProviderKind::Ollama;
    std::string   base_url;      // e.g. "https://api.openai.com/v1"
    std::string   api_key;       // empty for Ollama
    std::string   model;         // e.g. "gpt-4o", "claude-opus-4-7", "llama3"
    ModelParams   params;
    int           timeout_ms = 30000;

    // Parse from environment variables (JFSH_PROVIDER, JFSH_API_KEY, etc.).
    static ProviderConfig from_env();

    // Parse from a config file section.
    static ProviderConfig from_map(const HashTable<std::string>& kv);

    // Defaults for each provider kind.
    static ProviderConfig default_openai    (std::string model = "gpt-4o");
    static ProviderConfig default_anthropic (std::string model = "claude-opus-4-7");
    static ProviderConfig default_ollama    (std::string model = "llama3");
    static ProviderConfig default_openrouter(std::string model = "openai/gpt-4o");
};

// ---------------------------------------------------------------------------
// Message — one turn in a chat conversation
// ---------------------------------------------------------------------------
enum class Role : std::uint8_t { System, User, Assistant, Tool };

struct Message {
    Role        role;
    std::string content;
    std::string name;    // optional: tool call name
};

// ---------------------------------------------------------------------------
// ChatRequest — a full chat completion request
// ---------------------------------------------------------------------------
struct ChatRequest {
    ProviderConfig           config;
    std::vector<Message>     messages;
    bool                     stream = false;  // streaming responses
};

// ---------------------------------------------------------------------------
// ChatResponse — the provider's reply
// ---------------------------------------------------------------------------
struct ChatResponse {
    std::string content;         // assistant message text
    int         prompt_tokens    = 0;
    int         response_tokens  = 0;
    std::string finish_reason;   // "stop", "length", etc.
};

} // namespace jfsh
