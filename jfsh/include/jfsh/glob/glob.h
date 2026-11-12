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

#include <jfsh/core/options.h>
#include <jfsh/core/types.h>

namespace jfsh {

enum GlobFlags : std::uint32_t {
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

enum class GlobQual : std::uint8_t {
  None,
  RegularFile,   // .
  Directory,     // /
  SymLink,       // @
  Executable,    // *
  Readable,      // r
  Writable,      // w
  SetUID,        // s
  SetGID,        // S
  Sticky,        // t
  BlockDevice,   // %b
  CharDevice,    // %c
  NamedPipe,     // p
  Socket,        // =
  MinSize,       // Lk+N
  MaxSize,       // Lk-N
  MinAge,        // m+N
  MaxAge,        // m-N
  OwnedByUID,    // u:N
  OwnedByGID,    // g:N
};

struct GlobQualifier {
  GlobQual type   = GlobQual::None;
  long     value  = 0;
  bool     negate = false;
};

struct GlobPattern {
  std::string              raw;
  GlobFlags                flags = GLOB_NONE;
  std::vector<GlobQualifier> quals;

  static GlobPattern compile(std::string_view pat, GlobFlags flags = GLOB_NONE);
};

// A pattern compiled once and matched many times.  Syntax, in the escaped
// form the expander produces (a `\` makes the next character literal):
//   *  ?  [abc] [a-z] [!x] [^x] [[:alpha:]]   (a|b)   <lo-hi>
// and, with `extended`, zsh's EXTENDED_GLOB operators:
//   x#  (zero or more x)   x##  (one or more)   ^pat  (not pat)   a~b  (a but not b)
struct CompiledPattern {
  struct Node;
  struct Impl;

  static CompiledPattern compile(std::string_view pattern,
                                 bool case_insensitive = false,
                                 bool extended = false);

  // Whole-string match.
  bool matches(std::string_view str) const;
  // Length of the shortest (or longest) prefix of `str` that matches, or
  // npos when none does.
  std::size_t match_prefix(std::string_view str, bool longest) const;

private:
  std::shared_ptr<const Impl> mImpl;
};

// Expands `pat` against the file system.  `pat.raw` is in the escaped form
// described above; qualifiers like `(.)` or `(N)` at the end are honoured.
// Returns the sorted matches; with none, an error under NOMATCH, nothing
// under NULL_GLOB, and otherwise the pattern itself with escapes removed.
Result<std::vector<std::string>> glob_expand(
  const GlobPattern& pat,
  const OptionState& opts);

Result<std::vector<std::string>> expand_args(
  std::vector<std::string> args,
  const OptionState&       opts);

bool pattern_match(
  std::string_view pattern,
  std::string_view str,
  bool case_insensitive = false,
  bool extended = false) noexcept;

bool ext_pattern_match(
  std::string_view pattern,
  std::string_view str) noexcept;

// True when `s` contains an unescaped pattern character.
bool has_glob_chars(std::string_view s, bool extended = false) noexcept;

// Removes the `\` escapes of a pattern, leaving its literal text.
std::string glob_unescape(std::string_view s);

}  // namespace jfsh

#endif  // JFSH_GLOB_GLOB_H
