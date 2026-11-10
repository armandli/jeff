#ifndef JFSH_EDITOR_COMPLETION_HH
#define JFSH_EDITOR_COMPLETION_HH
// jfsh/editor/completion.hh — tab-completion engine.
//
// Mirrors zsh's Completion/ subsystem (compsys).

#include <cstdint>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <jfsh/core/param.hh>
#include <jfsh/core/types.hh>

namespace jfsh {

struct CompCandidate {
  std::string display;
  std::string insert;
  std::string description;
  int         priority = 0;
};

struct CompContext {
  std::string line;
  std::size_t cursor = 0;
  std::string word;
  std::string prefix;
  std::string suffix;

  std::vector<std::string> args;
  int                      current_arg = 0;

  std::string command;
};

using CompletionFn = std::function<std::vector<CompCandidate>(const CompContext&)>;

struct CompletionEngine {
  explicit CompletionEngine(const ParamTable& params);

  std::vector<CompCandidate> complete(std::string_view line, std::size_t cursor);

  void compdef(std::string command, CompletionFn fn);

  void set_default_completer(CompletionFn fn) {
    mDefaultFn = std::move(fn);
  }

  static std::vector<CompCandidate> complete_files(const CompContext&);
  static std::vector<CompCandidate> complete_commands(const CompContext&);
  static std::vector<CompCandidate> complete_variables(
    const CompContext&, const ParamTable&);
  static std::vector<CompCandidate> complete_options(
    const CompContext&, std::string_view optstring);

protected:
  CompContext build_context(std::string_view line, std::size_t cursor) const;

private:
  const ParamTable&       mParams;
  HashTable<CompletionFn> mCompdefTable;
  CompletionFn            mDefaultFn;
};

}  // namespace jfsh

#endif  // JFSH_EDITOR_COMPLETION_HH
