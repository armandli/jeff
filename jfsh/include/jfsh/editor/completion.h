#ifndef JFSH_EDITOR_COMPLETION_H
#define JFSH_EDITOR_COMPLETION_H
// jfsh/editor/completion.h — tab-completion engine.
//
// Mirrors zsh's Completion/ subsystem (compsys), in much smaller form.
// The word at the cursor is found by a small shell tokenizer (quotes,
// separators, redirections, command position), then completed by context:
// after `$` parameters, after `~` user names, in command position
// commands (builtins, functions, aliases, reserved words, executables in
// $PATH), after `cd` directories, otherwise files.  `compdef fn cmd` hands
// a command's arguments to a shell function, which adds matches with
// `compadd` (or the _files / _directories helpers).

#include <cstdint>
#include <ctime>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct CompCandidate {
  str  word;            // the match, unquoted (the part after the result's start)
  str  display;         // shown in a listing; the word when empty
  str  description;
  str  suffix = " ";    // added after a unique match: " ", "/", "}" or nothing
  char type   = 0;      // LIST_TYPES marker: / * @ | = or 0
};

struct CompContext {
  str       line;
  s::size_t cursor = 0;
  s::size_t word_start = 0;   // where the current word starts in `line`
  s::size_t word_end   = 0;   // and ends (past the cursor, to a blank)
  str       word;             // raw text from word_start to the cursor
  str       prefix;           // that text unquoted
  str       suffix;           // raw text from the cursor to word_end
  char      quote = 0;        // the open quote the word is in: ' or "

  vec<str> args;              // the command's words so far, unquoted
  int      current_arg = 0;   // index in args of the word at the cursor
  str      command;           // args[0], or empty in command position
  bool     command_position = false;
  bool     redirection      = false;   // the word is a redirection target
};

struct CompletionResult {
  s::size_t start = 0;        // the text [start, end) is replaced
  s::size_t end   = 0;
  str       prefix;           // what the matches complete, unquoted
  char      quote = 0;        // the word's open quote
  vec<CompCandidate> matches;
};

using CompletionFn = func<vec<CompCandidate>(const CompContext&)>;

// What the engine needs from the shell.
struct CompletionSources {
  // Builtins, functions, aliases and reserved words.
  func<vec<str>()> command_names;
  // Runs shell function `fn` for a compdef'd command; it adds matches
  // with compadd while running.
  func<void(const str& fn, const CompContext&)> call_function;
};

struct CompletionEngine {
  explicit CompletionEngine(const ParamTable& params) : mParams(params) {}

  CompletionResult complete(strv line, s::size_t cursor);

  // compdef: `fn` (a shell function) completes `command`'s arguments.
  void compdef(str command, str fn) { mCompdefs[s::move(command)] = s::move(fn); }
  void compdef_remove(const str& command) { mCompdefs.erase(command); }
  // A completer written in C++ (tests, built-in specs).
  void compdef_fn(str command, CompletionFn fn) { mBuiltinDefs[s::move(command)] = s::move(fn); }
  const s::map<str, str>& compdefs() const noexcept { return mCompdefs; }

  void set_sources(CompletionSources src) { mSources = s::move(src); }

  CompContext build_context(strv line, s::size_t cursor) const;

  // While a compdef function runs: the matches compadd adds, and the
  // context they are for.
  bool collecting() const noexcept { return mCollect != nullptr; }
  void add(CompCandidate c) { if (mCollect) mCollect->push_back(s::move(c)); }
  const CompContext* current() const noexcept { return mCurrent; }

  // Files matching the end of `prefix` (after its last /), in the
  // directory it names (~ and ~user expanded).  `dirs_only` keeps
  // directories, `exec_only` executables and directories; `pattern`
  // (a glob) keeps the files that match it, and directories.
  vec<CompCandidate> complete_files(strv prefix, bool dirs_only = false,
                                    bool exec_only = false, strv pattern = {}) const;
  vec<CompCandidate> complete_commands(strv prefix);
  vec<CompCandidate> complete_parameters(strv prefix) const;
  vec<CompCandidate> complete_users(strv prefix) const;

  // Quotes `w` for the line: backslashes, or inside an open ' or " what
  // that quote needs.
  static str quote_word(strv w, char quote);
  // Drops "" and duplicate words, then sorts.
  static void tidy(vec<CompCandidate>& v);
  // Forgets the cached $PATH listing (rehash).
  void rehash() { mPathCache.clear(); }

private:
  const ParamTable&       mParams;
  CompletionSources       mSources;
  s::map<str, str>        mCompdefs;
  HashTable<CompletionFn> mBuiltinDefs;
  vec<CompCandidate>*     mCollect = nullptr;
  const CompContext*      mCurrent = nullptr;

  // The executables in $PATH, per directory with its mtime.
  struct DirCache {
    str       dir;
    s::time_t mtime = 0;
    vec<str>  names;
  };
  str           mPathCacheKey;
  vec<DirCache> mPathCache;
};

}  // namespace jfsh

#endif  // JFSH_EDITOR_COMPLETION_H
