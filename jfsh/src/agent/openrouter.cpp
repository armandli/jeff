#include <jfsh/agent/openrouter.hh>

namespace jfsh {

OpenRouterBackend::OpenRouterBackend(ProviderConfig cfg)
    : delegate_([&] {
        ProviderConfig oc = cfg;
        oc.base_url = std::string(kBaseUrl);
        return oc;
      }())
    , cfg_(std::move(cfg)) {}

Result<ChatResponse> OpenRouterBackend::chat(const ChatRequest& req) {
    ChatRequest r = req;
    r.config.base_url = std::string(kBaseUrl);
    // Add OpenRouter-specific headers via extra_headers in the OpenAI backend
    return delegate_.chat(r);
}

Result<ChatResponse> OpenRouterBackend::chat_stream(const ChatRequest& req,
                                                     StreamCallback cb) {
    ChatRequest r = req;
    r.config.base_url = std::string(kBaseUrl);
    return delegate_.chat_stream(r, cb);
}

} // namespace jfsh
