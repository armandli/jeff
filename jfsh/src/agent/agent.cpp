#include <cstdio>

#include <jfsh/agent/agent.hh>
#include <jfsh/agent/anthropic.hh>
#include <jfsh/agent/ollama.hh>
#include <jfsh/agent/openai.hh>
#include <jfsh/agent/openrouter.hh>

namespace jfsh {

std::unique_ptr<AgentBackend> make_backend(const ProviderConfig& cfg) {
  switch (cfg.kind) {
    case ProviderKind::OpenAI:
      return std::make_unique<OpenAIBackend>(cfg);
    break; case ProviderKind::Anthropic:
      return std::make_unique<AnthropicBackend>(cfg);
    break; case ProviderKind::Ollama:
      return std::make_unique<OllamaBackend>(cfg);
    break; case ProviderKind::OpenRouter:
      return std::make_unique<OpenRouterBackend>(cfg);
    break; default:
      return std::make_unique<OllamaBackend>(cfg);
  }
}

std::unique_ptr<Agent> Agent::create(ProviderConfig cfg) {
  auto backend = make_backend(cfg);
  return std::make_unique<Agent>(std::move(backend), std::move(cfg));
}

Agent::Agent(std::unique_ptr<AgentBackend> backend, ProviderConfig cfg)
  : mBackend(std::move(backend))
  , mCfg(std::move(cfg))
  , mSystemPrompt("You are a helpful assistant integrated into jfsh, "
                  "a zsh-compatible shell. Help the user with tasks, "
                  "answer questions, and assist with shell scripting.") {}

void Agent::reset() { mHistory.clear(); }

void Agent::set_system_prompt(std::string prompt) {
  mSystemPrompt = std::move(prompt);
}

std::vector<Message> Agent::build_messages(std::string_view user_input) const {
  std::vector<Message> msgs;
  if (not mSystemPrompt.empty())
    msgs.push_back({Role::System, mSystemPrompt});
  int start = std::max(0, static_cast<int>(mHistory.size()) - mMaxTurns);
  for (int i = start; i < static_cast<int>(mHistory.size()); ++i) {
    msgs.push_back({Role::User,
        mHistory[static_cast<std::size_t>(i)].user_text});
    msgs.push_back({Role::Assistant,
        mHistory[static_cast<std::size_t>(i)].assistant_text});
  }
  msgs.push_back({Role::User, std::string(user_input)});
  return msgs;
}

Result<std::string> Agent::handle(std::string_view user_input, StreamCallback cb) {
  ChatRequest req;
  req.config   = mCfg;
  req.messages = build_messages(user_input);
  req.stream   = (cb != nullptr);

  Result<ChatResponse> resp;
  if (cb) {
    resp = mBackend->chat_stream(req, cb);
  } else {
    resp = mBackend->chat(req);
    if (resp) std::fputs(resp->content.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
  }

  if (not resp) return std::unexpected(resp.error());

  mHistory.push_back({std::string(user_input), resp->content});
  return resp->content;
}

}  // namespace jfsh
