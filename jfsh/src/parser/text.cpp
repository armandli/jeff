#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>

#include <jfsh/core/aliases.h>
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
  static_assert(s::size(kNames) == WC_MAX);
  return op < WC_MAX ? kNames[op] : "?";
}

// Joins the names of the set flags with `|`, or returns "-" when none are set.
static str flag_names(
    s::initializer_list<s::pair<bool, const char*>> flags) {
  str out;
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
      return type < s::size(kArms) ? kArms[type] : "?";
    }
    case WC_FUNCDEF:
      return type == WC_FUNCDEF_ANON ? "anon" : "named";
    case WC_COND: {
      static const char* const kConds[] = {"not", "and", "or", "unary", "binary", "str"};
      return type < s::size(kConds) ? kConds[type] : "?";
    }
    default:
      return nullptr;
  }
}

str disassemble(const CompiledProgram& prog) {
  str out;
  for (WordCode w : prog.prog) {
    WordCode op = wc_code(w);
    out += opcode_name(op);
    switch (op) {
      case WC_LIST: {
        WordCode f = wc_list_flags(w);
        out += ' ' + flag_names({{f & WC_LIST_ASYNC, "async"},
                                 {f & WC_LIST_END,   "end"}});
        out += " skip=" + s::to_string(wc_list_skip(w));
      }
      break; case WC_SUBLIST: {
        WordCode f    = wc_sublist_flags(w);
        WordCode type = f & WC_SUBLIST_TYPE;
        out += ' ' + flag_names({{type == WC_SUBLIST_AND, "and"},
                                 {type == WC_SUBLIST_OR,  "or"},
                                 {f & WC_SUBLIST_NOT,     "not"}});
        out += " skip=" + s::to_string(wc_sublist_skip(w));
      }
      break; case WC_ASSIGN:
        out += ' ' + flag_names({{wc_data(w) & WC_ASSIGN_APPEND, "append"},
                                 {wc_data(w) & WC_ASSIGN_ARRAY,  "array"}});
      break; case WC_PIPE: {
        WordCode n = wc_pipe_count(w);
        out += ' ' + s::to_string(n);
        str err;
        for (s::uint32_t i = 0; i + 1 < n; ++i)
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
          out += " fd=" + s::to_string(wc_redir_fd(w));
        if (wc_redir_literal(w)) out += " literal";
      }
      break; case WC_STRING: {
        WordCode idx = wc_data(w);
        out += idx < prog.strs.size() ? " \"" + prog.strs[idx] + '"'
                                      : " <bad index " + s::to_string(idx) + '>';
      }
      break; case WC_IF: case WC_WHILE: case WC_FOR: case WC_CASE:
      case WC_SELECT: case WC_REPEAT: case WC_SUBSH: case WC_CURSH:
      case WC_FUNCDEF: case WC_FOR_COND: case WC_TRY: case WC_ARITH: case WC_COND:
        if (auto* type = cmd_type_name(op, wc_cmd_type(w))) out += ' ' + str(type);
        out += " skip=" + s::to_string(wc_cmd_skip(w));
      break; case WC_END:
        ;
      break; default:
        out += ' ' + s::to_string(wc_data(w));
    }
    out += '\n';
  }
  return out;
}

// ---- One-line source text ---------------------------------------------------
//
// What job listings show for a pipeline: the command as written (words
// unexpanded), with compound commands on one line, as zsh's getjobtext().

namespace {

struct Decompiler {
  const CompiledProgram& p;
  s::size_t              pc;

  bool     at_end() const noexcept { return pc >= p.prog.size(); }
  WordCode peek()   const noexcept { return at_end() ? WC_END : p.prog[pc]; }
  WordCode fetch()        noexcept { return at_end() ? WC_END : p.prog[pc++]; }
  str next_str() {
    WordCode w = fetch();
    WordCode i = wc_data(w);
    return i < p.strs.size() ? p.strs[i] : str{};
  }

  str words() {
    WordCode n = wc_data(fetch());  // WC_WORDS
    str out;
    for (WordCode i = 0; i < n; ++i) {
      if (i) out += ' ';
      out += next_str();
    }
    return out;
  }

  // list* WC_END
  str body() {
    str out;
    while (not at_end() and wc_code(peek()) == WC_LIST) {
      WordCode  w   = fetch();
      s::size_t end = pc + wc_list_skip(w);
      str       s   = sublist();
      pc = end;
      if (not out.empty()) out += out.ends_with('&') ? " " : "; ";
      out += s;
      if (wc_list_flags(w) & WC_LIST_ASYNC) out += " &";
    }
    if (wc_code(peek()) == WC_END) ++pc;
    return out;
  }

  str sublist() {
    str out;
    for (;;) {
      WordCode  w   = fetch();  // WC_SUBLIST
      WordCode  f   = wc_sublist_flags(w);
      s::size_t end = pc + wc_sublist_skip(w);
      if (f & WC_SUBLIST_NOT) out += "! ";
      out += pipeline();
      pc = end;
      WordCode type = f & WC_SUBLIST_TYPE;
      if (type == WC_SUBLIST_END or at_end()) return out;
      out += type == WC_SUBLIST_AND ? " && " : " || ";
    }
  }

  vec<str> stages() {
    WordCode w = fetch();  // WC_PIPE
    WordCode n = wc_pipe_count(w);
    vec<str> out;
    for (WordCode i = 0; i < n and not at_end(); ++i) out.push_back(command());
    return out;
  }

  str pipeline() {
    WordCode w = peek();
    auto parts = stages();
    str out;
    for (s::size_t i = 0; i < parts.size(); ++i) {
      if (i) out += wc_pipe_stderr(w, static_cast<s::uint32_t>(i - 1)) ? " |& " : " | ";
      out += parts[i];
    }
    return out;
  }

  str redir() {
    WordCode    w      = fetch();
    str target = next_str();
    RedirType t      = wc_redir_type(w);
    str       fd     = wc_redir_has_fd(w) ? s::to_string(wc_redir_fd(w)) : "";
    switch (t) {
      case REDIR_READ:       return fd + "< " + target;
      case REDIR_WRITE:      return fd + "> " + target;
      case REDIR_APPEND:     return fd + ">> " + target;
      case REDIR_HEREDOC:    return fd + "<<EOF";
      case REDIR_HEREDOCD:   return fd + "<<-EOF";
      case REDIR_HERESTR:    return fd + "<<< " + target;
      case REDIR_READWRITE:  return fd + "<> " + target;
      case REDIR_CLOBBER:    return fd + ">| " + target;
      case REDIR_MERGE:      return fd + ">&" + target;
      case REDIR_MERGEOUT:   return "&> " + target;
      case REDIR_PROC_IN:    return "<(" + target + ")";
      case REDIR_PROC_OUT:   return ">(" + target + ")";
      case REDIR_PROC_EQ:    return "=(" + target + ")";
      case REDIR_CLOSE:      return fd + ">&-";
      case REDIR_IN_DUP:     return fd + "<&" + target;
      case REDIR_APPEND_ERR: return "&>> " + target;
      default:               return target;
    }
  }

  str assign() {
    WordCode    f     = wc_data(fetch());
    str name  = next_str();
    str value = next_str();
    str op    = (f & WC_ASSIGN_APPEND) ? "+=" : "=";
    if (f & WC_ASSIGN_ARRAY) return name + op + "(" + value + ")";
    return name + op + value;
  }

  str command() {
    vec<str> redirs;
    while (wc_code(peek()) == WC_REDIR) redirs.push_back(redir());
    WordCode op = wc_code(peek());
    vec<str> parts;
    if (op == WC_ASSIGN or op == WC_SIMPLE or op == WC_TYPESET) {
      vec<str> assigns;
      for (;;) {
        op = wc_code(peek());
        if      (op == WC_ASSIGN) assigns.push_back(assign());
        else if (op == WC_REDIR)  redirs.push_back(redir());
        else break;
      }
      parts = s::move(assigns);
      WordCode head = fetch();
      WordCode n    = wc_data(head);
      for (WordCode i = 0; i < n; ++i)
        parts.push_back(wc_code(peek()) == WC_ASSIGN ? assign() : next_str());
    } else {
      parts.push_back(compound());
    }
    for (auto& r : redirs) parts.push_back(s::move(r));
    str out;
    for (const auto& s : parts) {
      if (not out.empty()) out += ' ';
      out += s;
    }
    return out;
  }

  str cond() {
    WordCode w = fetch();
    switch (wc_cmd_type(w)) {
      case WC_COND_NOT: return "! " + cond();
      case WC_COND_AND: { auto a = cond(); return a + " && " + cond(); }
      case WC_COND_OR:  { auto a = cond(); return a + " || " + cond(); }
      case WC_COND_UNARY: { auto o = next_str(); return o + " " + next_str(); }
      case WC_COND_BINARY: {
        auto o = next_str(); auto l = next_str(); auto r = next_str();
        return l + " " + o + " " + r;
      }
      default: return next_str();
    }
  }

  str compound() {
    WordCode  head = fetch();
    s::size_t end  = pc + wc_cmd_skip(head);
    WordCode  type = wc_cmd_type(head);
    str       out;
    switch (wc_code(head)) {
      case WC_IF: {
        bool first = true;
        while (pc < end) {
          WordCode  b    = fetch();
          s::size_t bend = pc + wc_cmd_skip(b);
          if (wc_cmd_type(b) == WC_IF_IF) {
            out += first ? "if " : "; elif ";
            out += body();
            out += "; then " + body();
          } else {
            out += "; else " + body();
          }
          first = false;
          pc = bend;
        }
        out += "; fi";
      }
      break; case WC_WHILE: {
        out = type == WC_WHILE_UNTIL ? "until " : "while ";
        out += body();
        out += "; do " + body() + "; done";
      }
      break; case WC_FOR: case WC_SELECT: {
        out = wc_code(head) == WC_FOR ? "for " : "select ";
        out += words();
        if (type == WC_FOR_IN) out += " in " + words();
        out += "; do " + body() + "; done";
      }
      break; case WC_FOR_COND: {
        auto a = next_str(); auto b = next_str(); auto c = next_str();
        out = "for ((" + a + "; " + b + "; " + c + ")) do " + body() + "; done";
      }
      break; case WC_REPEAT: {
        out = "repeat " + next_str();
        out += "; do " + body() + "; done";
      }
      break; case WC_CASE: {
        out = "case " + next_str() + " in";
        while (pc < end) {
          WordCode  a    = fetch();
          s::size_t aend = pc + wc_cmd_skip(a);
          str       pats = words();
          for (char& c : pats) if (c == ' ') c = '|';
          out += " (" + pats + ") " + body();
          WordCode t = wc_cmd_type(a);
          out += t == WC_CASE_FALL ? " ;&" : t == WC_CASE_TESTNEXT ? " ;|" : " ;;";
          pc = aend;
        }
        out += " esac";
      }
      break; case WC_SUBSH: out = "( " + body() + " )";
      break; case WC_CURSH: out = "{ " + body() + " }";
      break; case WC_TRY: {
        out = "{ " + body();
        out += " } always { " + body() + " }";
      }
      break; case WC_FUNCDEF: {
        if (type == WC_FUNCDEF_ANON) {
          out = "() { " + body() + " }";
          str args = words();
          if (not args.empty()) out += " " + args;
        } else {
          out = words();
          out += " () { " + body() + " }";
        }
      }
      break; case WC_ARITH: {
        str e = next_str();
        auto b = e.find_first_not_of(" \t\n");
        auto l = e.find_last_not_of(" \t\n");
        out = "(( " + (b == str::npos ? str{} : e.substr(b, l - b + 1)) + " ))";
      }
      break; case WC_COND:  { --pc; out = "[[ " + cond() + " ]]"; }
      break; default: ;
    }
    pc = end;
    return out;
  }
};

}  // namespace

str pipeline_text(const CompiledProgram& prog, s::size_t pc) {
  return Decompiler{prog, pc}.pipeline();
}

vec<str> pipeline_stage_texts(const CompiledProgram& prog,
                              s::size_t pc) {
  return Decompiler{prog, pc}.stages();
}

str sublist_text(const CompiledProgram& prog, s::size_t pc) {
  return Decompiler{prog, pc}.sublist();
}

}  // namespace jfsh
