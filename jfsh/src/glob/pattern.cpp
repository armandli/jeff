#include <jfsh/glob/glob.h>

// Pattern matching and filename generation live in glob.cpp:
// CompiledPattern handles * ? [...] (a|b) <n-m> and, under EXTENDED_GLOB,
// x# x## ^pat a~b; glob_expand walks multi-segment paths, **/ and ***/,
// and the qualifiers . / @ * = p % r w x R W X ^ N D o/O{n,L,m,a,c} [n,m].
//
// TODO(glob/extglob): zsh's globbing flags `(#i)`, `(#l)`, `(#b)` and
//   approximate matching `(#a1)`; KSH_GLOB's `@(…)`, `*(…)`, `+(…)`.
//
// TODO(glob/quals): the remaining qualifiers — size `L±N`, age `m±N`,
//   ownership `u:name:`/`g:name:`, `(e:…:)` code, `(+fn)`, and `:` modifiers
//   inside the qualifier list (`*(:t)`).
//
// TODO(glob/cache): CompiledPattern is rebuilt for every directory; a small
//   cache would help `**/` walks of large trees.

namespace jfsh {
}  // namespace jfsh
