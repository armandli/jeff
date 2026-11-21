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

#include <jfsh/builtins/math.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>
#include <jfsh/glob/glob.h>

namespace jfsh {

struct Executor;
struct ExecContext;

struct Expander {
  Expander(Executor& exec, ExecContext& ctx) noexcept
    : mExec{exec}, mCtx{ctx} {}

  // Command arguments: every step, so one word may become several or none.
  // An unquoted `<(cmd)` or `>(cmd)` in the word as written, or `=(cmd)`
  // at its start, is a process substitution (Executor::procsubst); one
  // in quotes or in a parameter's value never is.
  Result<vec<str>> words(const vec<str>& words);

  // A word that stays one word: assignment values, here-strings, `>&N`
  // targets.  No splitting, braces or globbing; arrays are joined with
  // spaces.  With `assignment`, `~` also expands after `=` and `:`.
  // Process substitutions run as for words().
  Result<str> scalar(strv word, bool assignment = false);

  // Text that isn't a word of the script ($mailpath messages): no process
  // substitution.
  void no_process_substitution() noexcept { mProcSubst = false; }

  // The elements of `name=(…)`: `src` is the text between the parentheses,
  // split into shell words that are then expanded like arguments.
  Result<vec<str>> array_elements(strv src);

  // A here-document body: substitutions and backslash before $ ` \ and
  // newline only; quotes are ordinary characters.
  Result<str> heredoc(strv body);

  // A pattern for `case`, `[[ = ]]` and ${x#pat}: expansions done, and
  // characters that were quoted come back backslash-escaped.
  Result<str> pattern(strv word);

  // An arithmetic expression, as in `$((…))`: substitutions are expanded
  // first, then the text is evaluated.  Used by `(( ))`, `for ((;;))`,
  // `[[ -eq ]]`, and as an integer by `repeat` and subscripts.
  Result<MathNum>    math(strv expr);
  Result<s::int64_t> arith(strv expr);

  // Status of the last command substitution run, if any.
  opt<int> subst_status() const noexcept { return mSubstStatus; }

  // Internal: used by the scanner in subst.cpp.
  Executor&    exec() noexcept { return mExec; }
  ExecContext& ctx()  noexcept { return mCtx; }
  void         note_subst_status(int rc) noexcept { mSubstStatus = rc; }
  bool         process_substitution() const noexcept { return mProcSubst; }

private:
  // What glob qualifiers run through the shell, made on first use.
  const GlobHooks& glob_hooks();

  Executor&    mExec;
  ExecContext& mCtx;
  GlobHooks    mGlobHooks;
  opt<int>     mSubstStatus;
  bool         mProcSubst = true;
};

// Removes one level of shell quoting ('…', "…", $'…', backslash) from `w`
// without expanding anything; used by ${(Q)x}.
str remove_quotes(strv w);

// Splits `s` on the characters of `ifs` the way the shell splits unquoted
// expansions: runs of IFS whitespace are one separator, and leading and
// trailing whitespace makes no empty word.
vec<str> ifs_split(strv s, strv ifs);

}  // namespace jfsh

#endif  // JFSH_EXEC_SUBST_H
