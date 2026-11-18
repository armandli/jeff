#ifndef JFSH_MODULES_MATHFUNC_H
#define JFSH_MODULES_MATHFUNC_H
// jfsh/modules/mathfunc.h — the `zsh/mathfunc` module.
//
// Contributes zsh's C-math-library bindings as math functions resolvable
// from the arithmetic evaluator (`$((…))`, `(( ))`, `let`):
//   abs acos acosh asin asinh atan atan2 cbrt ceil cos cosh erf erfc exp
//   expm1 fabs float floor gamma hypot int lgamma log log10 log1p log2
//   logb pow rand48 rint signgam sin sinh sqrt tan tanh j0 j1 y0 y1.
// `int` and `float` cast; the rest compute.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_mathfunc_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_MATHFUNC_H
