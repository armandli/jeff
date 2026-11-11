#include <jfsh/parser/parser.hh>

// Decompiler: convert a CompiledProgram back to source text.
// Used by `functions` / `typeset -f` to print function bodies.
//
// TODO(text/decompile): Implement decompiletext() — walk the prog[] array and
//   reconstruct human-readable shell source from each WCOpcode:
//     WC_LIST     → join with ; or & or newline
//     WC_SUBLIST  → join with && or ||; prefix with !
//     WC_PIPE     → join with |
//     WC_SIMPLE   → print words from strs[]
//     WC_IF       → if cond; then body; [elif ...;] [else ...;] fi
//     WC_WHILE    → while cond; do body; done
//     WC_FOR      → for name in words; do body; done
//     WC_CASE     → case word in pat) body ;; esac
//     WC_SELECT   → select name in words; do body; done
//     WC_REPEAT   → repeat N; body; done
//     WC_SUBSH    → ( body )
//     WC_FUNCDEF  → function name { body }
//     WC_COND     → [[ ... ]]
//     WC_ARITH    → (( ... ))
//     WC_REDIR    → reconstruct < > >> << <<< <( etc.
//     WC_ASSIGN   → name=value
//   Indentation should increase inside compound commands.
//   String literals come from the strs[] table via WC_STRING indices.

namespace jfsh {
}  // namespace jfsh
