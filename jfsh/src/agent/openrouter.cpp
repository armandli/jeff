#include <jfsh/agent/openrouter.h>

namespace jfsh {

OpenRouterBackend::OpenRouterBackend(ProviderConfig cfg)
  : mDelegate([&] {
      ProviderConfig oc = cfg;
      oc.base_url = std::string(kBaseUrl);
      return oc;
    }())
  , mCfg(std::move(cfg)) {}

Result<ChatResponse> OpenRouterBackend::chat(const ChatRequest& req) {
  ChatRequest r = req;
  r.config.base_url = std::string(kBaseUrl);
  // Add OpenRouter-specific headers via extra_headers in the OpenAI backend
  return mDelegate.chat(r);
}

Result<ChatResponse> OpenRouterBackend::chat_stream(const ChatRequest& req,
                                                     StreamCallback cb) {
  ChatRequest r = req;
  r.config.base_url = std::string(kBaseUrl);
  return mDelegate.chat_stream(r, cb);
}

}  // namespace jfsh
