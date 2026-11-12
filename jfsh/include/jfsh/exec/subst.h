#ifndef JFSH_EXEC_SUBST_H
#define JFSH_EXEC_SUBST_H
// jfsh/exec/subst.h — word expansion.
//
// Mirrors zsh's subst.c (prefork/postfork) and the parts of glob.c and
// utils.c it calls.  A word goes through, in zsh's order:
//   1. parameter, command and arithmetic substitution, left to right
//      ($x ${x…} $(…) `…` $((…)) $[…]), with quote handling: '…', "…",
//      $'…' and backslash;
//   2. word splitting: unquoted arrays become separate words, unquoted
//      command substitutions are split on $IFS, scalars only with
//      SH_WORD_SPLIT or ${=x};
//   3. brace expansion {a,b} {1..5} {a..e} {01..10..2} (and {abc} under
//      BRACE_CCL), skipped under IGNORE_BRACES;
//   4. tilde (~ ~user ~+ ~-) and `=cmd` (EQUALS) expansion;
//   5. filename generation (globbing), through glob_expand;
//   6. quote removal, and removal of empty unquoted words.
// Characters that came from quotes or from an expansion are literal for
// steps 3–5, unless GLOB_SUBST or ${~x} asks otherwise.
//
// Process substitution `<(…)` `>(…)` `=(…)` is left to the executor.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <jfsh/core/types.h>

namespace jfsh {

struct Executor;
struct ExecContext;

struct Expander {
  Expander(Executor& exec, ExecContext& ctx) noexcept
    : mExec{exec}, mCtx{ctx} {}

  // Command arguments: every step, so one word may become several or none.
  Result<std::vector<std::string>> words(const std::vector<std::string>& words);

  // A word that stays one word: assignment values, here-strings, `>&N`
  // targets.  No splitting, braces or globbing; arrays are joined with
  // spaces.  With `assignment`, `~` also expands after `=` and `:`.
  Result<std::string> scalar(std::string_view word, bool assignment = false);

  // The elements of `name=(…)`: `src` is the text between the parentheses,
  // split into shell words that are then expanded like arguments.
  Result<std::vector<std::string>> array_elements(std::string_view src);

  // A here-document body: substitutions and backslash before $ ` \ and
  // newline only; quotes are ordinary characters.
  Result<std::string> heredoc(std::string_view body);

  // A pattern for `case`, `[[ = ]]` and ${x#pat}: expansions done, and
  // characters that were quoted come back backslash-escaped.
  Result<std::string> pattern(std::string_view word);

  // Status of the last command substitution run, if any.
  std::optional<int> subst_status() const noexcept { return mSubstStatus; }

  // Internal: used by the scanner in subst.cpp.
  Executor&    exec() noexcept { return mExec; }
  ExecContext& ctx()  noexcept { return mCtx; }
  void         note_subst_status(int rc) noexcept { mSubstStatus = rc; }

private:
  Executor&          mExec;
  ExecContext&       mCtx;
  std::optional<int> mSubstStatus;
};

// Removes one level of shell quoting ('…', "…", $'…', backslash) from `w`
// without expanding anything; used by ${(Q)x}.
std::string remove_quotes(std::string_view w);

// Splits `s` on the characters of `ifs` the way the shell splits unquoted
// expansions: runs of IFS whitespace are one separator, and leading and
// trailing whitespace makes no empty word.
std::vector<std::string> ifs_split(std::string_view s, std::string_view ifs);

}  // namespace jfsh

#endif  // JFSH_EXEC_SUBST_H
