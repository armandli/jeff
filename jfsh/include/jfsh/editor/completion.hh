#pragma once
// jfsh/editor/completion.hh — tab-completion engine.
//
// Mirrors zsh's Completion/ subsystem (compsys).  The completion engine
// runs the user-configured `compdef` and `compadd` / `_arguments` machinery,
// then passes the candidate list back to the ZLE editor.

#include <jfsh/core/types.hh>
#include <jfsh/core/param.hh>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace jfsh {

// ---------------------------------------------------------------------------
// CompCandidate — one completion result
// ---------------------------------------------------------------------------
struct CompCandidate {
    std::string display;    // shown in menu (may differ from inserted text)
    std::string insert;     // text to insert
    std::string description; // optional description shown in menu
    int         priority = 0; // higher = preferred
};

// ---------------------------------------------------------------------------
// CompContext — the state available to a completion function
// ---------------------------------------------------------------------------
struct CompContext {
    std::string line;       // current line buffer text
    std::size_t cursor;     // cursor position
    std::string word;       // the word being completed
    std::string prefix;     // characters before the word on the same token
    std::string suffix;     // characters after the cursor on the same token

    std::vector<std::string> args;  // all args of the current command
    int                      current_arg = 0;  // which arg is being completed

    std::string command;    // command name being completed for
};

// ---------------------------------------------------------------------------
// CompletionFn — a function that adds candidates for a given context
// ---------------------------------------------------------------------------
using CompletionFn = std::function<std::vector<CompCandidate>(const CompContext&)>;

// ---------------------------------------------------------------------------
// CompletionEngine
// ---------------------------------------------------------------------------
class CompletionEngine {
public:
    CompletionEngine(const ParamTable& params);

    // Main entry: given the current line + cursor, return candidates.
    std::vector<CompCandidate> complete(std::string_view line, std::size_t cursor);

    // Register a completion function for a command name ("compdef").
    void compdef(std::string command, CompletionFn fn);

    // Register a default fallback completer (file/path completion).
    void set_default_completer(CompletionFn fn) { default_fn_ = std::move(fn); }

    // Built-in completers
    static std::vector<CompCandidate> complete_files(const CompContext&);
    static std::vector<CompCandidate> complete_commands(const CompContext&);
    static std::vector<CompCandidate> complete_variables(const CompContext&, const ParamTable&);
    static std::vector<CompCandidate> complete_options(const CompContext&, std::string_view optstring);

private:
    const ParamTable&          params_;
    HashTable<CompletionFn>    compdef_table_;
    CompletionFn               default_fn_;

    CompContext build_context(std::string_view line, std::size_t cursor) const;
};

} // namespace jfsh
