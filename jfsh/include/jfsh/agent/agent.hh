#pragma once
// jfsh/agent/agent.hh — LLM agent that handles AGENT-classified commands.
//
// When the router classifies a line as AGENT, the Agent receives the full
// conversation context (history of commands + outputs in the session) and
// the user's input, sends a chat completion request to the configured LLM,
// and streams the response to the terminal.

#include <jfsh/agent/provider.hh>
#include <jfsh/core/types.hh>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jfsh {

// ---------------------------------------------------------------------------
// AgentTurn — one round of interaction stored in the session
// ---------------------------------------------------------------------------
struct AgentTurn {
    std::string user_text;
    std::string assistant_text;
};

// ---------------------------------------------------------------------------
// StreamCallback — called with each incremental response chunk
// ---------------------------------------------------------------------------
using StreamCallback = std::function<void(std::string_view chunk)>;

// ---------------------------------------------------------------------------
// AgentBackend — abstract interface one provider-specific implementation fills
// ---------------------------------------------------------------------------
struct AgentBackend {
    virtual ~AgentBackend() = default;

    // Send a chat completion request; return the full response.
    virtual Result<ChatResponse> chat(const ChatRequest& req) = 0;

    // Streaming variant: calls `cb` with each chunk, returns final accumulation.
    virtual Result<ChatResponse> chat_stream(const ChatRequest& req,
                                             StreamCallback cb) = 0;
};

// ---------------------------------------------------------------------------
// Agent — high-level agent that manages conversation history
// ---------------------------------------------------------------------------
class Agent {
public:
    // Construct with a provider config; chooses the right backend.
    static std::unique_ptr<Agent> create(ProviderConfig cfg);

    Agent(std::unique_ptr<AgentBackend> backend, ProviderConfig cfg);

    // Handle one AGENT-classified user input.
    // Streams the response to stdout (or the given callback).
    // Returns the full assistant reply text.
    Result<std::string> handle(std::string_view user_input,
                               StreamCallback cb = nullptr);

    // Clear the conversation history.
    void reset();

    // The system prompt (defaults to a helpful shell assistant prompt).
    void set_system_prompt(std::string prompt);

    // Current conversation turns.
    const std::vector<AgentTurn>& history() const noexcept { return history_; }

    // Max turns to keep in context (older turns are dropped).
    void set_max_turns(int n) noexcept { max_turns_ = n; }

    // Reload provider config (e.g. after user changes JFSH_MODEL).
    void set_config(ProviderConfig cfg) { cfg_ = std::move(cfg); }

private:
    std::unique_ptr<AgentBackend> backend_;
    ProviderConfig                cfg_;
    std::string                   system_prompt_;
    std::vector<AgentTurn>        history_;
    int                           max_turns_ = 20;

    std::vector<Message> build_messages(std::string_view user_input) const;
};

// ---------------------------------------------------------------------------
// Factory function — create a backend from a ProviderConfig
// ---------------------------------------------------------------------------
std::unique_ptr<AgentBackend> make_backend(const ProviderConfig& cfg);

} // namespace jfsh
