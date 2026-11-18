#ifndef JFSH_MODULES_EXAMPLE_H
#define JFSH_MODULES_EXAMPLE_H
// jfsh/modules/example.h — the `zsh/example` module (M10 polish).
//
// A documented minimal module that exists as a reference for third-party
// module authors and to exercise every ModuleFeatures contribution type
// (builtin, math function, binary condition, feature name) in tests.
//
// Builtin:   `example [-M FN] [-m MATH]`
//              -M FN    exits 0 if FN is a loaded math function
//              -m EXPR  passes if math_eval(EXPR) is non-zero
//              (no flags) prints "jfsh zsh/example"
// MathFunc:  example(x)         → x + 42
// Cond:      [[ s -example-match p ]]  — true iff s contains p literally

#include <jfsh/modules/module.h>

namespace jfsh {

void register_example_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_EXAMPLE_H
