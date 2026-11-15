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

// ---- One-line source text ---------------------------------------------------
//
// What job listings show for a pipeline: the command as written (words
// unexpanded), with compound commands on one line, as zsh's getjobtext().

namespace {

struct Decompiler {
  const CompiledProgram& p;
  std::size_t            pc;

  bool     at_end() const noexcept { return pc >= p.prog.size(); }
  WordCode peek()   const noexcept { return at_end() ? WC_END : p.prog[pc]; }
  WordCode fetch()        noexcept { return at_end() ? WC_END : p.prog[pc++]; }
  std::string str() {
    WordCode w = fetch();
    WordCode i = wc_data(w);
    return i < p.strs.size() ? p.strs[i] : std::string{};
  }

  std::string words() {
    WordCode n = wc_data(fetch());  // WC_WORDS
    std::string out;
    for (WordCode i = 0; i < n; ++i) {
      if (i) out += ' ';
      out += str();
    }
    return out;
  }

  // list* WC_END
  std::string body() {
    std::string out;
    while (not at_end() and wc_code(peek()) == WC_LIST) {
      WordCode    w   = fetch();
      std::size_t end = pc + wc_list_skip(w);
      std::string s   = sublist();
      pc = end;
      if (not out.empty()) out += out.ends_with('&') ? " " : "; ";
      out += s;
      if (wc_list_flags(w) & WC_LIST_ASYNC) out += " &";
    }
    if (wc_code(peek()) == WC_END) ++pc;
    return out;
  }

  std::string sublist() {
    std::string out;
    for (;;) {
      WordCode    w   = fetch();  // WC_SUBLIST
      WordCode    f   = wc_sublist_flags(w);
      std::size_t end = pc + wc_sublist_skip(w);
      if (f & WC_SUBLIST_NOT) out += "! ";
      out += pipeline();
      pc = end;
      WordCode type = f & WC_SUBLIST_TYPE;
      if (type == WC_SUBLIST_END or at_end()) return out;
      out += type == WC_SUBLIST_AND ? " && " : " || ";
    }
  }

  std::vector<std::string> stages() {
    WordCode w = fetch();  // WC_PIPE
    WordCode n = wc_pipe_count(w);
    std::vector<std::string> out;
    for (WordCode i = 0; i < n and not at_end(); ++i) out.push_back(command());
    return out;
  }

  std::string pipeline() {
    WordCode w = peek();
    auto parts = stages();
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i) out += wc_pipe_stderr(w, static_cast<unsigned>(i - 1)) ? " |& " : " | ";
      out += parts[i];
    }
    return out;
  }

  std::string redir() {
    WordCode    w      = fetch();
    std::string target = str();
    RedirType   t      = wc_redir_type(w);
    std::string fd     = wc_redir_has_fd(w) ? std::to_string(wc_redir_fd(w)) : "";
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

  std::string assign() {
    WordCode    f     = wc_data(fetch());
    std::string name  = str();
    std::string value = str();
    std::string op    = (f & WC_ASSIGN_APPEND) ? "+=" : "=";
    if (f & WC_ASSIGN_ARRAY) return name + op + "(" + value + ")";
    return name + op + value;
  }

  std::string command() {
    std::vector<std::string> redirs;
    while (wc_code(peek()) == WC_REDIR) redirs.push_back(redir());
    WordCode op = wc_code(peek());
    std::vector<std::string> parts;
    if (op == WC_ASSIGN or op == WC_SIMPLE or op == WC_TYPESET) {
      std::vector<std::string> assigns;
      for (;;) {
        op = wc_code(peek());
        if      (op == WC_ASSIGN) assigns.push_back(assign());
        else if (op == WC_REDIR)  redirs.push_back(redir());
        else break;
      }
      parts = std::move(assigns);
      WordCode head = fetch();
      WordCode n    = wc_data(head);
      for (WordCode i = 0; i < n; ++i)
        parts.push_back(wc_code(peek()) == WC_ASSIGN ? assign() : str());
    } else {
      parts.push_back(compound());
    }
    for (auto& r : redirs) parts.push_back(std::move(r));
    std::string out;
    for (const auto& s : parts) {
      if (not out.empty()) out += ' ';
      out += s;
    }
    return out;
  }

  std::string cond() {
    WordCode w = fetch();
    switch (wc_cmd_type(w)) {
      case WC_COND_NOT: return "! " + cond();
      case WC_COND_AND: { auto a = cond(); return a + " && " + cond(); }
      case WC_COND_OR:  { auto a = cond(); return a + " || " + cond(); }
      case WC_COND_UNARY: { auto o = str(); return o + " " + str(); }
      case WC_COND_BINARY: {
        auto o = str(); auto l = str(); auto r = str();
        return l + " " + o + " " + r;
      }
      default: return str();
    }
  }

  std::string compound() {
    WordCode    head = fetch();
    std::size_t end  = pc + wc_cmd_skip(head);
    WordCode    type = wc_cmd_type(head);
    std::string out;
    switch (wc_code(head)) {
      case WC_IF: {
        bool first = true;
        while (pc < end) {
          WordCode    b    = fetch();
          std::size_t bend = pc + wc_cmd_skip(b);
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
        auto a = str(); auto b = str(); auto c = str();
        out = "for ((" + a + "; " + b + "; " + c + ")) do " + body() + "; done";
      }
      break; case WC_REPEAT: {
        out = "repeat " + str();
        out += "; do " + body() + "; done";
      }
      break; case WC_CASE: {
        out = "case " + str() + " in";
        while (pc < end) {
          WordCode    a    = fetch();
          std::size_t aend = pc + wc_cmd_skip(a);
          std::string pats = words();
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
          std::string args = words();
          if (not args.empty()) out += " " + args;
        } else {
          out = words();
          out += " () { " + body() + " }";
        }
      }
      break; case WC_ARITH: {
        std::string e = str();
        auto b = e.find_first_not_of(" \t\n");
        auto l = e.find_last_not_of(" \t\n");
        out = "(( " + (b == std::string::npos ? std::string{} : e.substr(b, l - b + 1)) + " ))";
      }
      break; case WC_COND:  { --pc; out = "[[ " + cond() + " ]]"; }
      break; default: ;
    }
    pc = end;
    return out;
  }
};

}  // namespace

std::string pipeline_text(const CompiledProgram& prog, std::size_t pc) {
  return Decompiler{prog, pc}.pipeline();
}

std::vector<std::string> pipeline_stage_texts(const CompiledProgram& prog,
                                              std::size_t pc) {
  return Decompiler{prog, pc}.stages();
}

std::string sublist_text(const CompiledProgram& prog, std::size_t pc) {
  return Decompiler{prog, pc}.sublist();
}

}  // namespace jfsh
