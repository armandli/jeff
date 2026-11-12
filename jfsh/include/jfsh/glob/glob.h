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

bool has_glob_chars(std::string_view s) noexcept;

}  // namespace jfsh

#endif  // JFSH_GLOB_GLOB_H
