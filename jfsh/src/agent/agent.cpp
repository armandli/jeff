#include <jfsh/agent/agent.h>

#include <cstdio>

#include <jfsh/agent/anthropic.h>
#include <jfsh/agent/ollama.h>
#include <jfsh/agent/openai.h>
#include <jfsh/agent/openrouter.h>
#include <jfsh/core/aliases.h>

namespace jfsh {

uptr<AgentBackend> make_backend(const ProviderConfig& cfg) {
  switch (cfg.kind) {
    case ProviderKind::OpenAI:
      return s::make_unique<OpenAIBackend>(cfg);
    break; case ProviderKind::Anthropic:
      return s::make_unique<AnthropicBackend>(cfg);
    break; case ProviderKind::Ollama:
      return s::make_unique<OllamaBackend>(cfg);
    break; case ProviderKind::OpenRouter:
      return s::make_unique<OpenRouterBackend>(cfg);
    break; default:
      return s::make_unique<OllamaBackend>(cfg);
  }
}

uptr<Agent> Agent::create(ProviderConfig cfg) {
  auto backend = make_backend(cfg);
  return s::make_unique<Agent>(s::move(backend), s::move(cfg));
}

vec<Message> Agent::build_messages(strv user_input) const {
  vec<Message> msgs;
  if (not mSystemPrompt.empty())
    msgs.push_back({Role::System, mSystemPrompt});
  int start = s::max(0, static_cast<int>(mHistory.size()) - mMaxTurns);
  for (int i = start; i < static_cast<int>(mHistory.size()); ++i) {
    msgs.push_back({Role::User,
        mHistory[static_cast<s::size_t>(i)].user_text});
    msgs.push_back({Role::Assistant,
        mHistory[static_cast<s::size_t>(i)].assistant_text});
  }
  msgs.push_back({Role::User, str(user_input)});
  return msgs;
}

Result<str> Agent::handle(strv user_input, StreamCallback cb) {
  ChatRequest req;
  req.config   = mCfg;
  req.messages = build_messages(user_input);
  req.stream   = (cb != nullptr);

  Result<ChatResponse> resp;
  if (cb) {
    resp = mBackend->chat_stream(req, cb);
  } else {
    resp = mBackend->chat(req);
    if (resp) s::fputs(resp->content.c_str(), stdout);
    s::fputc('\n', stdout);
    s::fflush(stdout);
  }

  if (not resp) return s::unexpected(resp.error());

  mHistory.push_back({str(user_input), resp->content});
  return resp->content;
}

}  // namespace jfsh
