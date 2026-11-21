#ifndef JFSH_GLOB_GLOB_H
#define JFSH_GLOB_GLOB_H
// jfsh/glob/glob.h — pathname globbing and pattern matching.
//
// Mirrors zsh's glob.c / pattern.c.  Supports:
//   Basic:   * ? [...]
//   Extended (EXTENDEDGLOB): ^ ~ # ## (zsh)
//   GLOBDOTS, GLOBCOMPLETE, NULLGLOB, NOMATCH options
//   Glob qualifiers: (.) files only, (/) dirs, (*) executables, etc.
//   Recursive glob: **/ (GLOBRECURSE)

#include <cstdint>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

enum GlobFlags : s::uint32_t {
  GLOB_NONE     = 0,
  GLOB_EXTGLOB  = 1 << 0,
  GLOB_DOTFILES = 1 << 1,
  GLOB_NOCASE   = 1 << 2,
  GLOB_NULLGLOB = 1 << 3,
  GLOB_NOMATCH  = 1 << 4,
  GLOB_MARK     = 1 << 5,
  GLOB_NUMERIC  = 1 << 6,
  GLOB_RECURSE  = 1 << 7,
};

// What glob qualifiers need from the shell: running code (`e:code:`,
// `+func`) and applying modifiers (`:t`, `:s/a/b/`).
struct GlobHooks {
  // Runs `code` (with `is_func`, calls the function it names) with $REPLY
  // set to `file`.  Null when it fails; else the names to use instead
  // ($reply, or $REPLY when changed), or none to keep `file`.
  func<opt<vec<str>>(strv code, bool is_func, const str& file)> eval;
  // Applies `modifiers` (`:t:r`, with the colons) to `file`.
  func<Result<str>(strv modifiers, const str& file)> modify;
};

struct GlobPattern {
  str                    raw;
  GlobFlags              flags = GLOB_NONE;

  static GlobPattern compile(strv pat, GlobFlags flags = GLOB_NONE);
};

// Whether the locale's characters may take several bytes (UTF-8).
bool locale_multibyte() noexcept;

// A pattern compiled once and matched many times.  Syntax, in the escaped
// form the expander produces (a `\` makes the next character literal):
//   *  ?  [abc] [a-z] [!x] [^x] [[:alpha:]]   (a|b)   <lo-hi>
// and, with `extended`, zsh's EXTENDED_GLOB operators:
//   x#  (zero or more x)   x##  (one or more)   ^pat  (not pat)   a~b  (a but not b)
//   (#i) (#l) (#I) (#b) (#B) (#m) (#M) (#aN) (#s) (#e) (#cN,M)  globbing flags
// What a match found: the extent of each (#b) group, npos when it took
// no part.
struct PatternMatch {
  vec<s::pair<s::size_t, s::size_t>> groups;
};

struct CompiledPattern {
  struct Node;
  struct Impl;

  // With `multibyte`, `?` and `[…]` match a UTF-8 character, not a byte.
  static CompiledPattern compile(strv pattern,
                                 bool case_insensitive = false,
                                 bool extended = false,
                                 bool multibyte = locale_multibyte());

  // Whole-string match; `info` gets the backreferences.
  bool matches(strv text, PatternMatch* info = nullptr) const;
  // Whether text[b, e) matches, with (#s) and (#e) still at the start and
  // end of the whole `text` (${x/a(#e)/…} tries each part of x); the
  // backreferences in `info` count from b.
  bool matches(strv text, s::size_t b, s::size_t e, PatternMatch* info = nullptr) const;
  // Length of the shortest (or longest) prefix of `text` that matches, or
  // npos when none does.
  s::size_t match_prefix(strv text, bool longest, PatternMatch* info = nullptr) const;
  // The same for the part of `text` from b, as matches() with a window.
  s::size_t match_prefix(strv text, s::size_t b, bool longest,
                         PatternMatch* info = nullptr) const;
  // (#b) groups, and (#m), which set $match … and $MATCH … after a match.
  bool backrefs() const noexcept;
  bool sets_match() const noexcept;

private:
  sptr<const Impl> mImpl;
};

// After `pat` matched `len` characters of `text` from `start`: with (#b)
// sets $match, $mbegin and $mend to the groups, with (#m) $MATCH, $MBEGIN
// and $MEND to the match (1-based, or 0-based under `ksh_arrays`).
void set_match_params(ParamTable& params, const CompiledPattern& pat, strv text,
                      s::size_t start, s::size_t len, const PatternMatch& m,
                      bool ksh_arrays = false);

// Expands `pat` against the file system.  `pat.raw` is in the escaped form
// described above; zsh's glob qualifiers at the end (`(.)`, `(N-/)`,
// `(#qf:g+w:)`, `(e:code:)`, `(:t)`, …) are honoured, those that run code
// or modify through `hooks`.  Returns the sorted matches; with none, an
// error under NOMATCH, nothing under NULL_GLOB, and otherwise the pattern
// itself with escapes removed.
Result<vec<str>> glob_expand(
  const GlobPattern& pat,
  const OptionState& opts,
  const GlobHooks*   hooks = nullptr);

Result<vec<str>> expand_args(
  vec<str>           args,
  const OptionState& opts);

bool pattern_match(
  strv pattern,
  strv text,
  bool case_insensitive = false,
  bool extended = false) noexcept;

bool ext_pattern_match(
  strv pattern,
  strv text) noexcept;

// zsh's sort order (sort.c), for globs and the (o) flag: the locale's
// collation, with runs of digits compared by value when `numeric`.  Ties
// break by byte value, so different strings never compare equal.
int collate(const str& a, const str& b, bool numeric = false);
// Sorts `v` in collate() order.
void collate_sort(vec<str>& v, bool numeric = false);
// The strxfrm() form of `s`: keys compare bytewise as collate() orders
// their strings without `numeric`, ties aside.
str collate_key(const str& s);

// True when `s` contains an unescaped pattern character.
bool has_glob_chars(strv s, bool extended = false) noexcept;

// Removes the `\` escapes of a pattern, leaving its literal text.
str glob_unescape(strv s);

}  // namespace jfsh

#endif  // JFSH_GLOB_GLOB_H
