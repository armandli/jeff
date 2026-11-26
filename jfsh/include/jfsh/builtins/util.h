#ifndef JFSH_BUILTINS_UTIL_H
#define JFSH_BUILTINS_UTIL_H
// jfsh/builtins/util.h — helpers shared by the builtins: option parsing,
// quoting for listings, and printing parameters the way zsh does.

#include <cstdint>
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

// Whether a character shows as itself: zsh's ZISPRINT for ASCII, and
// iswprint for the rest in a multibyte locale.
bool printable_char(char32_t c) noexcept;

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

// zsh's struct options as execbuiltin fills it from a builtin's option
// string ("nqrs:": letters, `:` after one that takes an argument): ind[c]
// is 1 for -c, 2 for +c, with the number of c's argument above (zsh's
// OPT_ISSET, OPT_MINUS, OPT_PLUS, OPT_HASARG, OPT_ARG).
struct ZOps {
  bool isset(char c)  const noexcept { return ind[idx(c)] != 0; }
  bool minus(char c)  const noexcept { return ind[idx(c)] & 1; }
  bool plus(char c)   const noexcept { return ind[idx(c)] & 2; }
  bool hasarg(char c) const noexcept { return ind[idx(c)] > 3; }
  const str& arg(char c) const noexcept { return args[(ind[idx(c)] >> 2) - 1]; }
  // OPT_ARG_SAFE: the argument, or null.
  const str* arg_safe(char c) const noexcept { return hasarg(c) ? &arg(c) : nullptr; }

  vec<str>    args;
  s::uint16_t ind[128]{};

protected:
  static s::size_t idx(char c) noexcept { return static_cast<unsigned char>(c) & 127u; }
};

// execbuiltin's BINF_ flags that change how options are read.
enum ZOpsFlags : s::uint32_t {
  ZOPS_PLUSOPTS = 1 << 0,   // +x is an option too
  ZOPS_KEEPNUM  = 1 << 1,   // -5 is an option, not an argument
  ZOPS_SKIPDASH = 1 << 2,   // a lone - is an argument
  ZOPS_DASHDASH = 1 << 3,   // BINF_DASHDASHVALID
};

// execbuiltin's option scan and argument count check for `args[0]`, with
// zsh's messages (`bad option: -x`, `argument expected: -x`, `not enough
// arguments`, `too many arguments`).  A null `optstr` takes no options,
// only drops a first `--`.  Returns the index of the first argument, or -1
// after an error.
int zsh_options(const vec<str>& args, const char* optstr, int minargs, int maxargs,
                ZOps& ops, s::uint32_t flags = 0);

// The value of `p` as typeset -p and set print it: `'a b'`, `( a b )`
// (`(  )` when empty), `( [k]=v )`.
str param_value_text(const Param& p);

// Parameters, sorted by name, that `pred` accepts (every scope's innermost).
vec<const Param*> sorted_params(const ParamTable& params,
                                const func<bool(const Param&)>& pred);

// The value of $HOME, or "" when unset.
str home_dir(const ParamTable& params);
// `dir` with $HOME (as a whole path component) replaced by `~`.
str tilde_abbrev(const ParamTable& params, strv dir);

}  // namespace jfsh

#endif  // JFSH_BUILTINS_UTIL_H
