#ifndef JFSH_CORE_STACK_H
#define JFSH_CORE_STACK_H
// jfsh/core/stack.h — how much of the thread's stack is left.
//
// The recursive parts of expansion (nested ${…}, arithmetic, brace and
// pattern nesting, the lexer's nested substitutions) stop with an error
// when the stack runs low instead of counting levels, so the depth that
// works follows the real frame sizes of the build.

namespace jfsh {

// True when less than a safety margin of the calling thread's stack is
// left: 512 KB, or a quarter of a smaller stack.  False when the stack
// bounds cannot be found.
bool stack_low() noexcept;

}  // namespace jfsh

#endif  // JFSH_CORE_STACK_H
