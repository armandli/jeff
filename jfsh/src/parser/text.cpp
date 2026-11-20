#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>

#include <jfsh/core/aliases.h>
#include <jfsh/parser/parser.h>

// Text from bytecode: the disassembler (tests, debugging), one-line
// command text (job listings) and multi-line function definitions
// (`functions`, `whence -f`).

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
        out += ' ' + flag_names({{f & WC_LIST_ASYNC,  "async"},
                                 {f & WC_LIST_DISOWN, "disown"},
                                 {f & WC_LIST_END,    "end"}});
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

// A redirection's fd as zsh prints it: not when it is the operator's
// default (0 for those that read, 1 for the rest).
str fd_text(WordCode w) {
  if (not wc_redir_has_fd(w)) return {};
  int fd = wc_redir_fd(w);
  switch (wc_redir_type(w)) {
    case REDIR_READ: case REDIR_READWRITE: case REDIR_HEREDOC:
    case REDIR_HEREDOCD: case REDIR_HERESTR: case REDIR_IN_DUP:
    case REDIR_PROC_IN:
      if (fd == 0) return {};
    break; case REDIR_CLOSE:
    break; default:
      if (fd == 1) return {};
  }
  return s::to_string(fd);
}

// `<&-` or `>&-` (the target holds which), with the fd unless it is that
// operator's default, as zsh prints them.
str close_text(const str& fd, const str& target) {
  bool in = target == "<";
  if (fd == (in ? "0" : "1")) return in ? "<&-" : ">&-";
  return fd + (in ? "<&-" : ">&-");
}

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
      if (wc_list_flags(w) & WC_LIST_ASYNC)
        out += (wc_list_flags(w) & WC_LIST_DISOWN) ? " &|" : " &";
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
    str       fd     = fd_text(w);
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
      case REDIR_PROC_IN:    return fd + "< <(" + target + ")";
      case REDIR_PROC_OUT:   return fd + "> >(" + target + ")";
      case REDIR_CLOSE:      return close_text(fd, target);
      case REDIR_IN_DUP:     return fd + "<&" + target;
      case REDIR_APPEND_ERR: return "&>> " + target;
      case REDIR_APPEND_CLOBBER:     return fd + ">>| " + target;
      case REDIR_MERGEOUT_CLOBBER:   return "&>| " + target;
      case REDIR_APPEND_ERR_CLOBBER: return "&>>| " + target;
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
      case WC_COND_MOD: {
        str words = next_str();
        for (WordCode k = 1; k < wc_cmd_skip(w); ++k) words += " " + next_str();
        return words;
      }
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


// ---- Multi-line source text ---------------------------------------------------
//
// Function definitions as `functions` and `whence -f` print them: one
// command per line, bodies indented by a tab per level, as zsh's text.c.

struct Pretty {
  const CompiledProgram& p;
  s::size_t              pc;
  str                    out;
  vec<str>               docs;   // here-document bodies waiting for a newline

  bool     at_end() const noexcept { return pc >= p.prog.size(); }
  WordCode peek()   const noexcept { return at_end() ? WC_END : p.prog[pc]; }
  WordCode fetch()        noexcept { return at_end() ? WC_END : p.prog[pc++]; }
  str next_str() {
    WordCode w = fetch();
    WordCode i = wc_data(w);
    return i < p.strs.size() ? p.strs[i] : str{};
  }
  str words() {
    WordCode n = wc_data(fetch());
    str o;
    for (WordCode i = 0; i < n; ++i) {
      if (i) o += ' ';
      o += next_str();
    }
    return o;
  }
  void flush_docs() {
    for (auto& d : docs) out += "\n" + d + "EOF";
    docs.clear();
  }
  void nl(int ind) {
    flush_docs();
    out += '\n';
    out.append(static_cast<s::size_t>(ind), '\t');
  }

  // list* WC_END: the first command where we are, the rest on new lines.
  void body(int ind) {
    bool first = true;
    while (not at_end() and wc_code(peek()) == WC_LIST) {
      WordCode  w   = fetch();
      s::size_t end = pc + wc_list_skip(w);
      if (not first) nl(ind);
      sublist(ind);
      pc = end;
      if (wc_list_flags(w) & WC_LIST_ASYNC)
        out += (wc_list_flags(w) & WC_LIST_DISOWN) ? " &|" : " &";
      first = false;
    }
    if (wc_code(peek()) == WC_END) ++pc;
  }

  void sublist(int ind) {
    for (;;) {
      WordCode  w   = fetch();
      WordCode  f   = wc_sublist_flags(w);
      s::size_t end = pc + wc_sublist_skip(w);
      if (f & WC_SUBLIST_NOT) out += "! ";
      pipeline(ind);
      pc = end;
      WordCode type = f & WC_SUBLIST_TYPE;
      if (type == WC_SUBLIST_END or at_end()) return;
      out += type == WC_SUBLIST_AND ? " && " : " || ";
    }
  }

  void pipeline(int ind) {
    WordCode w = fetch();
    WordCode n = wc_pipe_count(w);
    for (WordCode i = 0; i < n and not at_end(); ++i) {
      if (i) out += wc_pipe_stderr(w, i - 1) ? " |& " : " | ";
      command(ind);
    }
  }

  str redir() {
    WordCode  w      = fetch();
    str       target = next_str();
    RedirType t      = wc_redir_type(w);
    str       fd     = fd_text(w);
    switch (t) {
      case REDIR_READ:       return fd + "< " + target;
      case REDIR_WRITE:      return fd + "> " + target;
      case REDIR_APPEND:     return fd + ">> " + target;
      case REDIR_HEREDOC:
      case REDIR_HEREDOCD:   docs.push_back(target); return fd + "<<EOF";
      case REDIR_HERESTR:    return fd + "<<< " + target;
      case REDIR_READWRITE:  return fd + "<> " + target;
      case REDIR_CLOBBER:    return fd + ">| " + target;
      case REDIR_MERGE:      return fd + ">&" + target;
      case REDIR_MERGEOUT:   return "&> " + target;
      case REDIR_PROC_IN:    return fd + "< <(" + target + ")";
      case REDIR_PROC_OUT:   return fd + "> >(" + target + ")";
      case REDIR_CLOSE:      return close_text(fd, target);
      case REDIR_IN_DUP:     return fd + "<&" + target;
      case REDIR_APPEND_ERR: return "&>> " + target;
      case REDIR_APPEND_CLOBBER:     return fd + ">>| " + target;
      case REDIR_MERGEOUT_CLOBBER:   return "&>| " + target;
      case REDIR_APPEND_ERR_CLOBBER: return "&>>| " + target;
      default:               return target;
    }
  }

  str assign() {
    WordCode f     = wc_data(fetch());
    str      name  = next_str();
    str      value = next_str();
    str      op    = (f & WC_ASSIGN_APPEND) ? "+=" : "=";
    if (f & WC_ASSIGN_ARRAY) return name + op + "(" + value + ")";
    return name + op + value;
  }

  void command(int ind) {
    vec<str> redirs;
    while (wc_code(peek()) == WC_REDIR) redirs.push_back(redir());
    WordCode op = wc_code(peek());
    if (op == WC_ASSIGN or op == WC_SIMPLE or op == WC_TYPESET) {
      str line;
      for (;;) {
        op = wc_code(peek());
        if      (op == WC_ASSIGN) line += assign() + " ";
        else if (op == WC_REDIR)  redirs.push_back(redir());
        else break;
      }
      WordCode head = fetch();
      WordCode n    = wc_data(head);
      bool any_assign = false;
      for (WordCode i = 0; i < n; ++i) {
        if (i) line += ' ';
        if (wc_code(peek()) == WC_ASSIGN) {
          line += assign();
          any_assign = true;
        } else {
          line += next_str();
        }
      }
      // zsh leaves a blank after typeset's assignments.
      if (wc_code(head) == WC_TYPESET and any_assign) line += ' ';
      out += line;
    } else {
      compound(ind);
    }
    for (auto& r : redirs) out += " " + r;
  }

  str cond() {
    WordCode w = fetch();
    switch (wc_cmd_type(w)) {
      case WC_COND_NOT: return "! " + cond();
      case WC_COND_AND: { auto a = cond(); return a + " && " + cond(); }
      case WC_COND_OR:  { auto a = cond(); return a + " || " + cond(); }
      case WC_COND_UNARY: { auto o = next_str(); return o + " " + next_str(); }
      case WC_COND_MOD: {
        str words = next_str();
        for (WordCode k = 1; k < wc_cmd_skip(w); ++k) words += " " + next_str();
        return words;
      }
      case WC_COND_BINARY: {
        auto o = next_str(); auto l = next_str(); auto r = next_str();
        return l + " " + o + " " + r;
      }
      default: return "-n " + next_str();
    }
  }

  // `do` on a line of its own, then the body, then `done`.
  void do_done(int ind, bool indent_body = true) {
    nl(ind);
    out += "do";
    nl(indent_body ? ind + 1 : ind);
    body(indent_body ? ind + 1 : ind);
    nl(ind);
    out += "done";
  }

  void compound(int ind) {
    WordCode  head = fetch();
    s::size_t end  = pc + wc_cmd_skip(head);
    WordCode  type = wc_cmd_type(head);
    switch (wc_code(head)) {
      case WC_IF: {
        bool first = true;
        while (pc < end) {
          WordCode  b    = fetch();
          s::size_t bend = pc + wc_cmd_skip(b);
          if (wc_cmd_type(b) == WC_IF_IF) {
            if (not first) nl(ind);
            out += first ? "if " : "elif ";
            body(ind + 1);
            nl(ind);
            out += "then";
            nl(ind + 1);
            body(ind + 1);
          } else {
            nl(ind);
            out += "else";
            nl(ind + 1);
            body(ind + 1);
          }
          first = false;
          pc = bend;
        }
        nl(ind);
        out += "fi";
      }
      break; case WC_WHILE:
        out += type == WC_WHILE_UNTIL ? "until " : "while ";
        body(ind + 1);
        do_done(ind);
      break; case WC_FOR: case WC_SELECT: {
        bool select = wc_code(head) == WC_SELECT;
        out += select ? "select " : "for ";
        out += words();
        if (type == WC_FOR_IN) out += " in " + words();
        // zsh 5.9 prints a select body without the extra indent.
        do_done(ind, not select);
      }
      break; case WC_FOR_COND: {
        auto a = next_str(); auto b = next_str(); auto c = next_str();
        out += "for ((" + a + "; " + b + "; " + c + ")) do";
        nl(ind + 1);
        body(ind + 1);
        nl(ind);
        out += "done";
      }
      break; case WC_REPEAT:
        out += "repeat " + next_str();
        do_done(ind);
      break; case WC_CASE: {
        out += "case " + next_str() + " in";
        while (pc < end) {
          WordCode  a    = fetch();
          s::size_t aend = pc + wc_cmd_skip(a);
          str       pats = words();
          for (char& ch : pats) if (ch == ' ') ch = '|';
          nl(ind + 1);
          out += "(" + pats + ") ";
          body(ind + 2);
          WordCode t = wc_cmd_type(a);
          out += t == WC_CASE_FALL ? " ;&" : t == WC_CASE_TESTNEXT ? " ;|" : " ;;";
          pc = aend;
        }
        nl(ind);
        out += "esac";
      }
      break; case WC_SUBSH: case WC_CURSH: {
        bool sub = wc_code(head) == WC_SUBSH;
        out += sub ? "(" : "{";
        nl(ind + 1);
        body(ind + 1);
        nl(ind);
        out += sub ? ")" : "}";
      }
      break; case WC_TRY:
        out += "{";
        nl(ind + 1);
        body(ind + 1);
        nl(ind);
        out += "} always {";
        nl(ind + 1);
        body(ind + 1);
        nl(ind);
        out += "}";
      break; case WC_FUNCDEF:
        if (type == WC_FUNCDEF_ANON) {
          out += "() {";
          nl(ind + 1);
          body(ind + 1);
          nl(ind);
          out += "}";
          str args = words();
          if (not args.empty()) out += " " + args;
        } else {
          out += words() + " () {";
          nl(ind + 1);
          body(ind + 1);
          nl(ind);
          out += "}";
        }
      break; case WC_ARITH: {
        str e = next_str();
        auto b = e.find_first_not_of(" \t\n");
        auto l = e.find_last_not_of(" \t\n");
        out += "(( " + (b == str::npos ? str{} : e.substr(b, l - b + 1)) + " ))";
      }
      break; case WC_COND: --pc; out += "[[ " + cond() + " ]]";
      break; default: ;
    }
    pc = end;
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

namespace jfsh {

// A body that is one `{ … } redirs` command: what `f() { … } redirs`
// compiles to (Parser::wrap_body_redirs).
static bool redirected_body(const CompiledProgram& body) {
  const auto& p = body.prog;
  if (p.size() < 6 or wc_code(p[0]) != WC_LIST
      or not (wc_list_flags(p[0]) & WC_LIST_END)
      or 1 + wc_list_skip(p[0]) + 1 != p.size()
      or wc_code(p[1]) != WC_SUBLIST or wc_sublist_flags(p[1]) != WC_SUBLIST_END
      or wc_code(p[2]) != WC_PIPE or wc_pipe_count(p[2]) != 1)
    return false;
  s::size_t pc = 3;
  while (pc < p.size() and wc_code(p[pc]) == WC_REDIR) pc += 2;
  return pc > 3 and pc < p.size() and wc_code(p[pc]) == WC_CURSH;
}

str function_text(strv names, const CompiledProgram& body) {
  if (redirected_body(body)) {
    Pretty pr{body, 0, {}, {}};
    pr.out = str(names) + " () ";
    pr.body(0);
    pr.flush_docs();
    return pr.out;
  }
  Pretty pr{body, 0, {}, {}};
  pr.out = str(names) + " () {\n\t";
  pr.body(1);
  pr.flush_docs();
  pr.out += "\n}";
  return pr.out;
}

}  // namespace jfsh
