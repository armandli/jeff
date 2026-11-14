#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>

#include <jfsh/parser/parser.h>

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

static const char* opcode_name(WordCode op) noexcept {
  static const char* const kNames[] = {
    "END", "LIST", "SUBLIST", "PIPE", "REDIR", "SIMPLE", "TYPESET", "ASSIGN",
    "IF", "WHILE", "FOR", "CASE", "SELECT", "REPEAT", "SUBSH", "FUNCDEF",
    "FOR_COND", "ALWAYS", "TRY", "COND", "ARITH", "AUTOFN", "STRING",
    "CURSH", "WORDS",
  };
  static_assert(std::size(kNames) == WC_MAX);
  return op < WC_MAX ? kNames[op] : "?";
}

// Joins the names of the set flags with `|`, or returns "-" when none are set.
static std::string flag_names(
    std::initializer_list<std::pair<bool, const char*>> flags) {
  std::string out;
  for (auto [set, name] : flags) {
    if (not set) continue;
    if (not out.empty()) out += '|';
    out += name;
  }
  return out.empty() ? "-" : out;
}

// The type field of a compound command header, or nullptr when the
// construct has only one form.
static const char* cmd_type_name(WordCode op, WordCode type) noexcept {
  switch (op) {
    case WC_IF:
      return type == WC_IF_HEAD ? "head" : type == WC_IF_IF ? "if" : "else";
    case WC_WHILE:
      return type == WC_WHILE_UNTIL ? "until" : "while";
    case WC_FOR: case WC_SELECT:
      return type == WC_FOR_ARGS ? "args" : "in";
    case WC_CASE: {
      static const char* const kArms[] = {"head", ";;", ";&", ";|"};
      return type < std::size(kArms) ? kArms[type] : "?";
    }
    case WC_FUNCDEF:
      return type == WC_FUNCDEF_ANON ? "anon" : "named";
    case WC_COND: {
      static const char* const kConds[] = {"not", "and", "or", "unary", "binary", "str"};
      return type < std::size(kConds) ? kConds[type] : "?";
    }
    default:
      return nullptr;
  }
}

std::string disassemble(const CompiledProgram& prog) {
  std::string out;
  for (WordCode w : prog.prog) {
    WordCode op = wc_code(w);
    out += opcode_name(op);
    switch (op) {
      case WC_LIST: {
        WordCode f = wc_list_flags(w);
        out += ' ' + flag_names({{f & WC_LIST_ASYNC, "async"},
                                 {f & WC_LIST_END,   "end"}});
        out += " skip=" + std::to_string(wc_list_skip(w));
      }
      break; case WC_SUBLIST: {
        WordCode f    = wc_sublist_flags(w);
        WordCode type = f & WC_SUBLIST_TYPE;
        out += ' ' + flag_names({{type == WC_SUBLIST_AND, "and"},
                                 {type == WC_SUBLIST_OR,  "or"},
                                 {f & WC_SUBLIST_NOT,     "not"}});
        out += " skip=" + std::to_string(wc_sublist_skip(w));
      }
      break; case WC_ASSIGN:
        out += ' ' + flag_names({{wc_data(w) & WC_ASSIGN_APPEND, "append"},
                                 {wc_data(w) & WC_ASSIGN_ARRAY,  "array"}});
      break; case WC_PIPE: {
        WordCode n = wc_pipe_count(w);
        out += ' ' + std::to_string(n);
        std::string err;
        for (unsigned i = 0; i + 1 < n; ++i)
          err += wc_pipe_stderr(w, i) ? '&' : '|';
        if (not err.empty()) out += ' ' + err;
      }
      break; case WC_REDIR: {
        static const char* const NAMES[REDIR_TYPE_MAX] = {
          "<", ">", ">>", "<<", "<<-", "<<<", "<>", ">|",
          ">&N", "&>", "<(", ">(", "=(", ">&-", "<&N", "&>>",
        };
        RedirType t = wc_redir_type(w);
        out += ' ';
        out += t < REDIR_TYPE_MAX ? NAMES[t] : "?";
        if (wc_redir_has_fd(w))
          out += " fd=" + std::to_string(wc_redir_fd(w));
        if (wc_redir_literal(w)) out += " literal";
      }
      break; case WC_STRING: {
        WordCode idx = wc_data(w);
        out += idx < prog.strs.size() ? " \"" + prog.strs[idx] + '"'
                                      : " <bad index " + std::to_string(idx) + '>';
      }
      break; case WC_IF: case WC_WHILE: case WC_FOR: case WC_CASE:
      case WC_SELECT: case WC_REPEAT: case WC_SUBSH: case WC_CURSH:
      case WC_FUNCDEF: case WC_FOR_COND: case WC_TRY: case WC_ARITH: case WC_COND:
        if (auto* type = cmd_type_name(op, wc_cmd_type(w))) out += ' ' + std::string(type);
        out += " skip=" + std::to_string(wc_cmd_skip(w));
      break; case WC_END:
        ;
      break; default:
        out += ' ' + std::to_string(wc_data(w));
    }
    out += '\n';
  }
  return out;
}

}  // namespace jfsh
