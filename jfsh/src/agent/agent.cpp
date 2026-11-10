#include <jfsh/agent/agent.hh>
#include <jfsh/agent/openai.hh>
#include <jfsh/agent/anthropic.hh>
#include <jfsh/agent/ollama.hh>
#include <jfsh/agent/openrouter.hh>
#include <cstdio>

namespace jfsh {

std::unique_ptr<AgentBackend> make_backend(const ProviderConfig& cfg) {
    switch (cfg.kind) {
        case ProviderKind::OpenAI:      return std::make_unique<OpenAIBackend>(cfg);
        case ProviderKind::Anthropic:   return std::make_unique<AnthropicBackend>(cfg);
        case ProviderKind::Ollama:      return std::make_unique<OllamaBackend>(cfg);
        case ProviderKind::OpenRouter:  return std::make_unique<OpenRouterBackend>(cfg);
    }
    return std::make_unique<OllamaBackend>(cfg);
}

std::unique_ptr<Agent> Agent::create(ProviderConfig cfg) {
    auto backend = make_backend(cfg);
    return std::make_unique<Agent>(std::move(backend), std::move(cfg));
}

Agent::Agent(std::unique_ptr<AgentBackend> backend, ProviderConfig cfg)
    : backend_(std::move(backend))
    , cfg_(std::move(cfg))
    , system_prompt_("You are a helpful assistant integrated into jfsh, a zsh-compatible shell. "
                     "Help the user with tasks, answer questions, and assist with shell scripting.") {}

void Agent::reset() { history_.clear(); }

void Agent::set_system_prompt(std::string prompt) { system_prompt_ = std::move(prompt); }

std::vector<Message> Agent::build_messages(std::string_view user_input) const {
    std::vector<Message> msgs;
    if (!system_prompt_.empty())
        msgs.push_back({Role::System, system_prompt_});
    // Add history (trimmed to max_turns_)
    int start = std::max(0, static_cast<int>(history_.size()) - max_turns_);
    for (int i = start; i < static_cast<int>(history_.size()); ++i) {
        msgs.push_back({Role::User,      history_[static_cast<std::size_t>(i)].user_text});
        msgs.push_back({Role::Assistant, history_[static_cast<std::size_t>(i)].assistant_text});
    }
    msgs.push_back({Role::User, std::string(user_input)});
    return msgs;
}

Result<std::string> Agent::handle(std::string_view user_input, StreamCallback cb) {
    ChatRequest req;
    req.config   = cfg_;
    req.messages = build_messages(user_input);
    req.stream   = (cb != nullptr);

    Result<ChatResponse> resp;
    if (cb) {
        resp = backend_->chat_stream(req, cb);
    } else {
        resp = backend_->chat(req);
        if (resp) std::fputs(resp->content.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    if (!resp) return std::unexpected(resp.error());

    history_.push_back({std::string(user_input), resp->content});
    return resp->content;
}

} // namespace jfsh
