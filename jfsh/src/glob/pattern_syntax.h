#ifndef JFSH_GLOB_PATTERN_SYNTAX_H
#define JFSH_GLOB_PATTERN_SYNTAX_H
// What filename generation needs of the pattern parser (pattern.cpp),
// apart from the matchers' internals.

#include <cstddef>

#include <jfsh/core/aliases.h>

namespace jfsh::pattern {

// Where the `[…]` at p[i] ends, npos when it is never closed (then `[` is
// text).
s::size_t bracket_end(strv p, s::size_t i);

}  // namespace jfsh::pattern

#endif  // JFSH_GLOB_PATTERN_SYNTAX_H
