#ifndef JFSH_MODULES_MATHFUNC_H
#define JFSH_MODULES_MATHFUNC_H
// jfsh/modules/mathfunc.h — the `zsh/mathfunc` module (a port of zsh's
// Src/Modules/mathfunc.c).
//
// The math functions of zsh's mftab, resolvable from `$((…))`, `(( ))` and
// `let` once the module is loaded: abs acos acosh asin asinh atan (atan(y,x)
// is atan2) atanh cbrt ceil copysign cos cosh erf erfc exp expm1 fabs float
// floor fmod gamma hypot ilogb int j0 j1 jn ldexp lgamma log log10 log1p
// log2 logb nextafter rand48 (a string function: rand48(seed-param)) rint
// scalb signgam sin sinh sqrt tan tanh y0 y1 yn.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_mathfunc_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_MATHFUNC_H
