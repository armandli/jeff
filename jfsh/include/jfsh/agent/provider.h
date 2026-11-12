#ifndef JFSH_AGENT_PROVIDER_H
#define JFSH_AGENT_PROVIDER_H
// jfsh/agent/provider.h — LLM provider configuration.

#include <cstdint>

#include <optional>
#include <string>
#include <variant>

#include <jfsh/core/types.h>

namespace jeff::json { class Value; }

namespace jfsh {

enum class ProviderKind : std::uint8_t {
  OpenAI,
  Anthropic,
  Ollama,
  OpenRouter,
};

struct ModelParams {
  float              temperature = 0.7f;
  float              top_p       = 1.0f;
  int                max_tokens  = 4096;
  std::optional<int> seed;
};

struct ProviderConfig {
  ProviderKind kind       = ProviderKind::Ollama;
  std::string  base_url;
  std::string  api_key;
  std::string  model;
  ModelParams  params;
  int          timeout_ms = 30000;

  static ProviderConfig from_env();
  static ProviderConfig from_map(const HashTable<std::string>& kv);

  static ProviderConfig default_openai    (std::string model = "gpt-4o");
  static ProviderConfig default_anthropic (std::string model = "claude-opus-4-7");
  static ProviderConfig default_ollama    (std::string model = "llama3");
  static ProviderConfig default_openrouter(std::string model = "openai/gpt-4o");
};

enum class Role : std::uint8_t { System, User, Assistant, Tool };

struct Message {
  Role        role;
  std::string content;
  std::string name;
};

struct ChatRequest {
  ProviderConfig       config;
  std::vector<Message> messages;
  bool                 stream = false;
};

struct ChatResponse {
  std::string content;
  int         prompt_tokens   = 0;
  int         response_tokens = 0;
  std::string finish_reason;
};

// The provider's error message from a response body, if it is an error response.
// Handles {"error":{"message":"..."}} (OpenAI, Anthropic) and {"error":"..."} (Ollama).
std::optional<std::string> api_error_message(const jeff::json::Value& response);

}  // namespace jfsh

#endif  // JFSH_AGENT_PROVIDER_H
