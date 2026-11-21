#include <jfsh/glob/glob.h>

// Pattern matching and filename generation live in glob.cpp:
// CompiledPattern handles * ? [...] (a|b) a|b <n-m>, the globbing flags
// (#i #l #I #b #B #m #M #s #e #aN #cN,M) and, under EXTENDED_GLOB, x# x##
// ^pat a~b; glob_expand walks multi-segment paths, **/ and ***/, and
// applies zsh's glob qualifiers.
//
// TODO(glob/kshglob): KSH_GLOB's `@(…)`, `*(…)`, `+(…)`, `?(…)`, `!(…)`.

namespace jfsh {
}  // namespace jfsh
