#ifndef JFSH_EDITOR_COMPLETION_H
#define JFSH_EDITOR_COMPLETION_H
// jfsh/editor/completion.h — tab-completion engine.
//
// Mirrors zsh's Completion/ subsystem (compsys).

#include <cstdint>

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct CompCandidate {
  str display;
  str insert;
  str description;
  int priority = 0;
};

struct CompContext {
  str line;
  s::size_t cursor = 0;
  str word;
  str prefix;
  str suffix;

  vec<str> args;
  int      current_arg = 0;

  str command;
};

using CompletionFn = func<vec<CompCandidate>(const CompContext&)>;

struct CompletionEngine {
  explicit CompletionEngine(const ParamTable& params)
    : mParams(params), mDefaultFn(complete_files) {}

  vec<CompCandidate> complete(strv line, s::size_t cursor);

  void compdef(str command, CompletionFn fn) {
    mCompdefTable[s::move(command)] = s::move(fn);
  }

  void set_default_completer(CompletionFn fn) {
    mDefaultFn = s::move(fn);
  }

  static vec<CompCandidate> complete_files(const CompContext&);
  static vec<CompCandidate> complete_commands(const CompContext&);
  static vec<CompCandidate> complete_variables(
    const CompContext&, const ParamTable&);
  static vec<CompCandidate> complete_options(
    const CompContext&, strv optstring) { return {}; }

protected:
  CompContext build_context(strv line, s::size_t cursor) const;

private:
  [[maybe_unused]] const ParamTable& mParams;   // not used yet
  HashTable<CompletionFn> mCompdefTable;
  CompletionFn            mDefaultFn;
};

}  // namespace jfsh

#endif  // JFSH_EDITOR_COMPLETION_H
