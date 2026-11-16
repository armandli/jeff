#ifndef JFSH_BUILTINS_UTIL_H
#define JFSH_BUILTINS_UTIL_H
// jfsh/builtins/util.h — helpers shared by the builtins: option parsing,
// quoting for listings, and printing parameters the way zsh does.

#include <cstdio>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/param.h>

namespace jfsh {

struct ExecContext;

// Prints `jfsh: cmd: msg` on stderr; returns 1 for `return builtin_error(…)`.
int builtin_error(strv cmd, strv msg);

// A shell identifier: [A-Za-z_][A-Za-z0-9_]*.
bool is_identifier(strv s) noexcept;

// Quotes `s` for output that can be read back, as zsh's listings do: as
// is when it needs no quoting, otherwise in single quotes ('' empty), or
// as $'…' when it holds control characters.
str shell_quote(strv s);

// zsh's single-letter options: `-abc` sets a, b and c (`+abc` clears
// them); letters listed in `with_arg` take the rest of the word or the
// next word as their argument.  Parsing stops at the first word that isn't
// an option, at `--` (consumed) and at `-` (consumed: the end of the
// options).  `plus` says whether `+x` is allowed.
struct OptParse {
  struct Flag {
    char c;
    bool on;   // `-x` (true) or `+x`
    str  arg;  // for the letters in with_arg
  };
  vec<Flag> flags;
  s::size_t next = 1;   // index of the first argument after the options

  bool has(char c) const noexcept;
  bool has_plus(char c) const noexcept;
  const str* arg(char c) const noexcept;   // the last argument given to c
};

// Parses the options of `args[0]`.  Prints `bad option` / `argument
// expected` and returns false on an error.  With `numbers`, a word like
// `-2` is an argument, not options (fc's event numbers).
bool parse_options(const vec<str>& args, strv letters, strv with_arg,
                   OptParse& out, bool plus = false, bool numbers = false);

// The value of `p` as typeset -p and set print it: `'a b'`, `( a b )`,
// `( [k]=v )`.
str param_value_text(const Param& p);

// The typeset options that recreate `p`'s attributes: "-a", "-i", "-rx" …
// (empty for a plain scalar).
str param_type_flags(const Param& p);

// Parameters, sorted by name, that `pred` accepts (every scope's innermost).
vec<const Param*> sorted_params(const ParamTable& params,
                                const func<bool(const Param&)>& pred);

// The value of $HOME, or "" when unset.
str home_dir(const ParamTable& params);
// `dir` with $HOME (as a whole path component) replaced by `~`.
str tilde_abbrev(const ParamTable& params, strv dir);

}  // namespace jfsh

#endif  // JFSH_BUILTINS_UTIL_H
