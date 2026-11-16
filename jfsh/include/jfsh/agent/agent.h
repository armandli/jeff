#ifndef JFSH_AGENT_AGENT_H
#define JFSH_AGENT_AGENT_H
// jfsh/agent/agent.h — LLM agent that handles AGENT-classified commands.
//
// When the router classifies a line as AGENT, the Agent receives the full
// conversation context and the user's input, sends a chat completion request
// to the configured LLM, and streams the response to the terminal.

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/agent/provider.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct AgentTurn {
  str user_text;
  str assistant_text;
};

using StreamCallback = func<void(strv chunk)>;

struct AgentBackend {
  virtual ~AgentBackend() = default;

  virtual Result<ChatResponse> chat(const ChatRequest& req) = 0;
  virtual Result<ChatResponse> chat_stream(
    const ChatRequest& req, StreamCallback cb) = 0;
};

struct Agent {
  static uptr<Agent> create(ProviderConfig cfg);

  Agent(uptr<AgentBackend> backend, ProviderConfig cfg)
    : mBackend(s::move(backend))
    , mCfg(s::move(cfg))
    , mSystemPrompt("You are a helpful assistant integrated into jfsh, "
                    "a zsh-compatible shell. Help the user with tasks, "
                    "answer questions, and assist with shell scripting.") {}

  Result<str> handle(
    strv user_input, StreamCallback cb = nullptr);

  void reset() { mHistory.clear(); }
  void set_system_prompt(str prompt) { mSystemPrompt = s::move(prompt); }
  void set_config(ProviderConfig cfg) { mCfg = s::move(cfg); }
  void set_max_turns(int n) noexcept  { mMaxTurns = n; }

  const vec<AgentTurn>& history() const noexcept { return mHistory; }

protected:
  vec<Message> build_messages(strv user_input) const;

private:
  uptr<AgentBackend> mBackend;
  ProviderConfig     mCfg;
  str                mSystemPrompt;
  vec<AgentTurn>     mHistory;
  int                mMaxTurns = 20;
};

uptr<AgentBackend> make_backend(const ProviderConfig& cfg);

}  // namespace jfsh

#endif  // JFSH_AGENT_AGENT_H
