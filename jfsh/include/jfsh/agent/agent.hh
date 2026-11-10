#ifndef JFSH_AGENT_AGENT_HH
#define JFSH_AGENT_AGENT_HH
// jfsh/agent/agent.hh — LLM agent that handles AGENT-classified commands.
//
// When the router classifies a line as AGENT, the Agent receives the full
// conversation context and the user's input, sends a chat completion request
// to the configured LLM, and streams the response to the terminal.

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <jfsh/agent/provider.hh>
#include <jfsh/core/types.hh>

namespace jfsh {

struct AgentTurn {
  std::string user_text;
  std::string assistant_text;
};

using StreamCallback = std::function<void(std::string_view chunk)>;

struct AgentBackend {
  virtual ~AgentBackend() = default;

  virtual Result<ChatResponse> chat(const ChatRequest& req) = 0;
  virtual Result<ChatResponse> chat_stream(
    const ChatRequest& req, StreamCallback cb) = 0;
};

struct Agent {
  static std::unique_ptr<Agent> create(ProviderConfig cfg);

  Agent(std::unique_ptr<AgentBackend> backend, ProviderConfig cfg);

  Result<std::string> handle(
    std::string_view user_input, StreamCallback cb = nullptr);

  void reset();
  void set_system_prompt(std::string prompt);
  void set_config(ProviderConfig cfg) { mCfg = std::move(cfg); }
  void set_max_turns(int n) noexcept  { mMaxTurns = n; }

  const std::vector<AgentTurn>& history() const noexcept { return mHistory; }

protected:
  std::vector<Message> build_messages(std::string_view user_input) const;

private:
  std::unique_ptr<AgentBackend> mBackend;
  ProviderConfig                mCfg;
  std::string                   mSystemPrompt;
  std::vector<AgentTurn>        mHistory;
  int                           mMaxTurns = 20;
};

std::unique_ptr<AgentBackend> make_backend(const ProviderConfig& cfg);

}  // namespace jfsh

#endif  // JFSH_AGENT_AGENT_HH
