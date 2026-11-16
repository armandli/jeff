#ifndef JFSH_AGENT_PROVIDER_H
#define JFSH_AGENT_PROVIDER_H
// jfsh/agent/provider.h — LLM provider configuration.

#include <cstdint>

#include <optional>
#include <string>
#include <variant>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jeff::json { class Value; }

namespace jfsh {

enum class ProviderKind : s::uint8_t {
  OpenAI,
  Anthropic,
  Ollama,
  OpenRouter,
};

struct ModelParams {
  float    temperature = 0.7f;
  float    top_p       = 1.0f;
  int      max_tokens  = 4096;
  opt<int> seed;
};

struct ProviderConfig {
  ProviderKind kind       = ProviderKind::Ollama;
  str          base_url;
  str          api_key;
  str          model;
  ModelParams  params;
  int          timeout_ms = 30000;

  static ProviderConfig from_env();
  static ProviderConfig from_map(const HashTable<str>& kv);

  static ProviderConfig default_openai    (str model = "gpt-4o");
  static ProviderConfig default_anthropic (str model = "claude-opus-4-7");
  static ProviderConfig default_ollama    (str model = "llama3");
  static ProviderConfig default_openrouter(str model = "openai/gpt-4o");
};

enum class Role : s::uint8_t { System, User, Assistant, Tool };

struct Message {
  Role role;
  str  content;
  str name = {};
};

struct ChatRequest {
  ProviderConfig config;
  vec<Message>   messages;
  bool           stream = false;
};

struct ChatResponse {
  str content;
  int prompt_tokens   = 0;
  int response_tokens = 0;
  str finish_reason;
};

// The provider's error message from a response body, if it is an error response.
// Handles {"error":{"message":"..."}} (OpenAI, Anthropic) and {"error":"..."} (Ollama).
opt<str> api_error_message(const jeff::json::Value& response);

}  // namespace jfsh

#endif  // JFSH_AGENT_PROVIDER_H
