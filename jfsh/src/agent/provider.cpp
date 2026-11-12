#include <cstdlib>

#include <string_view>

#include <json.h>

#include <jfsh/agent/provider.h>

namespace jfsh {

ProviderConfig ProviderConfig::from_env() {
  ProviderConfig cfg;
  const char* provider = std::getenv("JFSH_PROVIDER");
  if (not provider or std::string(provider) == "ollama")
    cfg.kind = ProviderKind::Ollama;
  else if (std::string(provider) == "openai")
    cfg.kind = ProviderKind::OpenAI;
  else if (std::string(provider) == "anthropic")
    cfg.kind = ProviderKind::Anthropic;
  else if (std::string(provider) == "openrouter")
    cfg.kind = ProviderKind::OpenRouter;

  const char* key = std::getenv("JFSH_API_KEY");
  if (key) cfg.api_key = key;

  const char* model = std::getenv("JFSH_MODEL");
  if (model) cfg.model = model;

  const char* url = std::getenv("JFSH_BASE_URL");
  if (url) cfg.base_url = url;

  if (cfg.model.empty()) {
    switch (cfg.kind) {
      case ProviderKind::OpenAI:
        cfg.model = "gpt-4o";
      break; case ProviderKind::Anthropic:
        cfg.model = "claude-opus-4-7";
      break; case ProviderKind::Ollama:
        cfg.model = "llama3";
      break; case ProviderKind::OpenRouter:
        cfg.model = "openai/gpt-4o";
      break; default:;
    }
  }
  return cfg;
}

ProviderConfig ProviderConfig::from_map(const HashTable<std::string>& kv) {
  ProviderConfig cfg;
  if (auto it = kv.find("provider"); it != kv.end()) {
    std::string_view p = it->second;
    if (p == "openai")          cfg.kind = ProviderKind::OpenAI;
    else if (p == "anthropic")  cfg.kind = ProviderKind::Anthropic;
    else if (p == "openrouter") cfg.kind = ProviderKind::OpenRouter;
    else                        cfg.kind = ProviderKind::Ollama;
  }
  if (auto it = kv.find("api_key");  it != kv.end()) cfg.api_key  = it->second;
  if (auto it = kv.find("model");    it != kv.end()) cfg.model    = it->second;
  if (auto it = kv.find("base_url"); it != kv.end()) cfg.base_url = it->second;
  return cfg;
}

ProviderConfig ProviderConfig::default_openai(std::string model) {
  ProviderConfig c; c.kind = ProviderKind::OpenAI; c.model = std::move(model); return c;
}
ProviderConfig ProviderConfig::default_anthropic(std::string model) {
  ProviderConfig c; c.kind = ProviderKind::Anthropic; c.model = std::move(model); return c;
}
ProviderConfig ProviderConfig::default_ollama(std::string model) {
  ProviderConfig c; c.kind = ProviderKind::Ollama; c.model = std::move(model);
  c.base_url = "http://localhost:11434"; return c;
}
ProviderConfig ProviderConfig::default_openrouter(std::string model) {
  ProviderConfig c; c.kind = ProviderKind::OpenRouter; c.model = std::move(model); return c;
}

std::optional<std::string> api_error_message(const jeff::json::Value& response) {
  const jeff::json::Value* err = response.find("error");
  if (not err or err->is_null()) return std::nullopt;
  if (const std::string* s = err->as_string()) return *s;
  if (const jeff::json::Value* msg = err->find("message"))
    if (const std::string* s = msg->as_string()) return *s;
  return err->dump();
}

}  // namespace jfsh
