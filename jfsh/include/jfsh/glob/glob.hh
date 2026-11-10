#pragma once
// jfsh/glob/glob.hh — pathname globbing and pattern matching.
//
// Mirrors zsh's glob.c / pattern.c.  Supports:
//   Basic:   * ? [...]
//   Extended (EXTENDEDGLOB): ^ ~ # ## (zsh)
//   GLOBDOTS, GLOBCOMPLETE, NULLGLOB, NOMATCH options
//   Glob qualifiers: (.) files only, (/) dirs, (*) executables, etc.
//   Recursive glob: **/ (GLOBRECURSE)

#include <jfsh/core/types.hh>
#include <jfsh/core/options.hh>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jfsh {

// ---------------------------------------------------------------------------
// GlobFlags — per-expansion options
// ---------------------------------------------------------------------------
enum GlobFlags : std::uint32_t {
    GLOB_NONE       = 0,
    GLOB_EXTGLOB    = 1 << 0,  // zsh extended glob operators
    GLOB_DOTFILES   = 1 << 1,  // match leading dots
    GLOB_NOCASE     = 1 << 2,  // case-insensitive
    GLOB_NULLGLOB   = 1 << 3,  // return empty list (not error) on no match
    GLOB_NOMATCH    = 1 << 4,  // error on no match
    GLOB_MARK       = 1 << 5,  // append / to matched directories
    GLOB_NUMERIC    = 1 << 6,  // sort numerically
    GLOB_RECURSE    = 1 << 7,  // ** recursive
};

// ---------------------------------------------------------------------------
// Glob qualifier types (zsh glob qualifiers inside parentheses)
// ---------------------------------------------------------------------------
enum class GlobQual : std::uint8_t {
    None,
    RegularFile,     // .
    Directory,       // /
    SymLink,         // @
    Executable,      // *
    Readable,        // r
    Writable,        // w
    SetUID,          // s
    SetGID,          // S
    Sticky,          // t
    BlockDevice,     // %b
    CharDevice,      // %c
    NamedPipe,       // p
    Socket,          // =
    MinSize,         // Lk+N
    MaxSize,         // Lk-N
    MinAge,          // m+N
    MaxAge,          // m-N
    OwnedByUID,      // u:N
    OwnedByGID,      // g:N
};

struct GlobQualifier {
    GlobQual type   = GlobQual::None;
    long     value  = 0;
    bool     negate = false;
};

// ---------------------------------------------------------------------------
// GlobPattern — a compiled glob pattern ready for matching
// ---------------------------------------------------------------------------
struct GlobPattern {
    std::string              raw;        // original pattern string
    GlobFlags                flags = GLOB_NONE;
    std::vector<GlobQualifier> quals;    // zsh qualifiers

    static GlobPattern compile(std::string_view pat, GlobFlags flags = GLOB_NONE);
};

// ---------------------------------------------------------------------------
// Glob functions
// ---------------------------------------------------------------------------

// Expand a single glob pattern to sorted, deduplicated list of matches.
// Respects NULLGLOB / NOMATCH from opts.
Result<std::vector<std::string>> glob_expand(
    const GlobPattern&  pat,
    const OptionState&  opts);

// Expand all glob patterns in an argument list.
// Non-glob words pass through unchanged.
Result<std::vector<std::string>> expand_args(
    std::vector<std::string>  args,
    const OptionState&        opts);

// Pattern match: does `str` match `pattern`?  (Used by case, [[, etc.)
// Respects CASEMATCH option.
bool pattern_match(std::string_view pattern, std::string_view str,
                   bool case_insensitive = false,
                   bool extended = false) noexcept;

// Extended-glob match (uses zsh #/##/^/~ operators).
bool ext_pattern_match(std::string_view pattern, std::string_view str) noexcept;

// Check if a string contains any unquoted glob characters.
bool has_glob_chars(std::string_view s) noexcept;

} // namespace jfsh
