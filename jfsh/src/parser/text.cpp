#include <cctype>
#include <cstring>

#include <algorithm>
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
      static const char* const kArms[] = {"head", ";;", ";&", ";|",
                                          "?", ";; grouped", ";& grouped", ";| grouped"};
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
        if (idx < prog.strs.size()) {
          // The NUL in a here-document's or `{name}>`'s string shows as \0.
          out += " \"";
          for (char c : prog.strs[idx]) out += c == '\0' ? str("\\0") : str(1, c);
          out += '"';
        } else {
          out += " <bad index " + s::to_string(idx) + '>';
        }
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

// A here-document waiting for the end of the line: its body and the line
// that ends it.
struct HereDoc {
  str body, end;
};

// `text` in single quotes, as zsh's quotedzputs gives a here-document in a
// job's one-line text.
str single_quote(strv text) {
  str out = "'";
  for (char c : text) out += c == '\'' ? str("'\\''") : str(1, c);
  return out + "'";
}

// Reads the bytecode for both texts, and gives the parts they write the
// same way: words, redirections, case patterns and conditions.
struct Reader {
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

  str words(strv sep = " ") {
    WordCode n = wc_data(fetch());  // WC_WORDS
    str out;
    for (WordCode i = 0; i < n; ++i) {
      if (i) out += sep;
      out += next_str();
    }
    return out;
  }

  // A case arm's patterns, as zsh prints them: `a | b`, or `a|b` when they
  // were written as one group.
  str patterns(WordCode arm) {
    return words(wc_cmd_type(arm) & WC_CASE_GROUPED ? "|" : " | ");
  }

  // The connector before stage `i + 1` of pipeline `w`; zsh writes `|&` as
  // `2>&1 |`.
  static strv pipe_text(WordCode w, WordCode i) {
    return wc_pipe_stderr(w, i) ? " 2>&1 | " : " | ";
  }

  // A redirection.  A here-document is `<<DELIM` with its body and end
  // line added to `docs`, or, with no `docs` (one-line text), its body as
  // a here-string, as in zsh's job text.
  str redir(vec<HereDoc>* docs) {
    WordCode  w      = fetch();
    str       target = next_str();
    RedirType t      = wc_redir_type(w);
    str       fd     = fd_text(w);
    if (wc_redir_varid(w)) {
      auto [name, rest] = split_redir_varid(target);
      fd = "{" + str(name) + "}";
      target = str(rest);
    }
    switch (t) {
      case REDIR_READ:       return fd + "< " + target;
      case REDIR_WRITE:      return fd + "> " + target;
      case REDIR_APPEND:     return fd + ">> " + target;
      case REDIR_HEREDOC:
      case REDIR_HEREDOCD: {
        auto [delim, body] = split_heredoc(target);
        if (not docs) {
          strv text = body.ends_with('\n') ? body.substr(0, body.size() - 1) : body;
          return fd + "<<<" + single_quote(text);
        }
        bool quoted = false;
        docs->push_back({str(body), heredoc_delimiter(delim, quoted)});
        return fd + "<<" + str(delim);
      }
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
    if (f & WC_ASSIGN_ARRAY) return name + op + "(" + array_words(value) + ")";
    return name + op + value;
  }

  // An array's elements as zsh writes them: one space between words,
  // none inside the parentheses (`x=( a  b )` is `x=(a b)`).  Quotes,
  // backquotes, `$( … )`, `${ … }`, `$[ … ]`, `<( … )`, `>( … )` and
  // `=( … )` are kept as they are.
  static str array_words(strv raw) {
    str  out;
    char quote = 0;
    int  depth = 0;
    bool gap   = false;
    for (s::size_t i = 0; i < raw.size(); ++i) {
      char c = raw[i];
      if (not quote and depth == 0 and (c == ' ' or c == '\t' or c == '\n')) {
        gap = not out.empty();
        continue;
      }
      if (gap) out += ' ';
      gap = false;
      out += c;
      if (c == '\\' and quote != '\'' and i + 1 < raw.size()) out += raw[++i];
      else if (quote and c == quote) quote = 0;
      else if (quote) continue;
      else if (c == '\'' or c == '"' or c == '`') quote = c;
      else if ((c == '(' or c == '{' or c == '[')
               and (depth > 0 or (i > 0 and s::strchr(c == '[' ? "$" : "$<>=", raw[i - 1]))))
        ++depth;
      else if (depth > 0 and (c == ')' or c == '}' or c == ']')) --depth;
    }
    return out;
  }

  // The condition of `[[ … ]]`, with parentheses where zsh's text.c puts
  // them: around a `&&` under `||` (and the other way round) and around a
  // `!`, `&&` or `||` under `!`, so the text reads back the same.  A lone
  // word is `-n word` with `n_word`.  The right side of a chain is
  // followed in a loop, as the executor does.
  str cond(bool n_word) {
    str out;
    int closing = 0;
    auto open = [&](WordCode parent) {
      WordCode t = wc_cmd_type(peek());
      bool binary = t == WC_COND_AND or t == WC_COND_OR;
      if (parent == WC_COND_NOT ? binary or t == WC_COND_NOT : binary and t != parent) {
        out += "( ";
        return true;
      }
      return false;
    };
    for (;;) {
      WordCode w    = fetch();
      WordCode type = wc_cmd_type(w);
      if (type == WC_COND_NOT) {
        out += "! ";
        if (open(type)) ++closing;
        continue;
      }
      if (type == WC_COND_AND or type == WC_COND_OR) {
        bool paren = open(type);
        out += cond(n_word);
        if (paren) out += " )";
        out += type == WC_COND_AND ? " && " : " || ";
        if (open(type)) ++closing;
        continue;
      }
      switch (type) {
        case WC_COND_UNARY: {
          auto o = next_str();
          out += o + " " + next_str();
        }

        break; case WC_COND_MOD: {
          out += next_str();
          for (WordCode k = 1; k < wc_cmd_skip(w); ++k) out += " " + next_str();
        }

        break; case WC_COND_BINARY: {
          auto o = next_str();
          auto l = next_str();
          auto r = next_str();
          out += l + " " + o + " " + r;
        }

        break; default: out += (n_word ? "-n " : "") + next_str();
      }
      for (; closing > 0; --closing) out += " )";
      return out;
    }
  }
};

struct Decompiler : Reader {

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
      if (i) out += pipe_text(w, static_cast<WordCode>(i - 1));
      out += parts[i];
    }
    return out;
  }

  str command() {
    vec<str> redirs;
    while (wc_code(peek()) == WC_REDIR) redirs.push_back(redir(nullptr));
    WordCode op = wc_code(peek());
    vec<str> parts;
    if (op == WC_ASSIGN or op == WC_SIMPLE or op == WC_TYPESET) {
      vec<str> assigns;
      for (;;) {
        op = wc_code(peek());
        if      (op == WC_ASSIGN) assigns.push_back(assign());
        else if (op == WC_REDIR)  redirs.push_back(redir(nullptr));
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

  // zsh's job text (gettext2 without newlines) ends every list inside a
  // compound command with "; ", the keywords that open one too:
  // `( a; )`, `if a; then; b; fi`, `for x; do; b; done`.
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
            out += "; then; " + body();
          } else {
            out += "; else; " + body();
          }
          first = false;
          pc = bend;
        }
        out += "; fi";
      }
      break; case WC_WHILE: {
        out = type == WC_WHILE_UNTIL ? "until " : "while ";
        out += body();
        out += "; do; " + body() + "; done";
      }
      break; case WC_FOR: case WC_SELECT: {
        out = wc_code(head) == WC_FOR ? "for " : "select ";
        out += words();
        if (type == WC_FOR_IN) out += " in " + words();
        out += "; do; " + body() + "; done";
      }
      break; case WC_FOR_COND: {
        auto a = next_str(); auto b = next_str(); auto c = next_str();
        out = "for ((" + a + "; " + b + "; " + c + ")) do; " + body() + "; done";
      }
      break; case WC_REPEAT: {
        out = "repeat " + next_str();
        out += "; do; " + body() + "; done";
      }
      break; case WC_CASE: {
        out = "case " + next_str() + " in";
        while (pc < end) {
          WordCode  a    = fetch();
          s::size_t aend = pc + wc_cmd_skip(a);
          out += " (" + patterns(a) + ") " + body();
          WordCode t = wc_case_end(a);
          out += t == WC_CASE_FALL ? " ;&" : t == WC_CASE_TESTNEXT ? " ;|" : " ;;";
          pc = aend;
        }
        out += " esac";
      }
      break; case WC_SUBSH: out = "( " + body() + "; )";
      break; case WC_CURSH: out = "{ " + body() + "; }";
      break; case WC_TRY: {
        out = "{; " + body();
        out += "; } always {; " + body() + "; }";
      }
      break; case WC_FUNCDEF: {
        // A definition's body is left out.
        if (type == WC_FUNCDEF_ANON) {
          out = "() { ... }";
        } else {
          out = words();
          out += " () { ... }";
        }
      }
      break; case WC_ARITH: {
        str e = next_str();
        auto b = e.find_first_not_of(" \t\n");
        auto l = e.find_last_not_of(" \t\n");
        out = "(( " + (b == str::npos ? str{} : e.substr(b, l - b + 1)) + " ))";
      }
      break; case WC_COND:  { --pc; out = "[[ " + cond(false) + " ]]"; }
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

struct Pretty : Reader {
  str          out{};
  vec<HereDoc> docs{};   // here-documents waiting for a newline

  void flush_docs() {
    for (auto& d : docs) out += "\n" + d.body + d.end;
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
      if (i) out += pipe_text(w, i - 1);
      command(ind);
    }
  }

  void command(int ind) {
    vec<str> redirs;
    while (wc_code(peek()) == WC_REDIR) redirs.push_back(redir(&docs));
    WordCode op = wc_code(peek());
    if (op == WC_ASSIGN or op == WC_SIMPLE or op == WC_TYPESET) {
      str line;
      for (;;) {
        op = wc_code(peek());
        if      (op == WC_ASSIGN) line += assign() + " ";
        else if (op == WC_REDIR)  redirs.push_back(redir(&docs));
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
          nl(ind + 1);
          out += "(" + patterns(a) + ") ";
          body(ind + 2);
          WordCode t = wc_case_end(a);
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
      break; case WC_COND: --pc; out += "[[ " + cond(true) + " ]]";
      break; default: ;
    }
    pc = end;
  }
};

}  // namespace

str pipeline_text(const CompiledProgram& prog, s::size_t pc) {
  return Decompiler{{prog, pc}}.pipeline();
}

vec<str> pipeline_stage_texts(const CompiledProgram& prog,
                              s::size_t pc) {
  return Decompiler{{prog, pc}}.stages();
}

str sublist_text(const CompiledProgram& prog, s::size_t pc) {
  // zsh names a background `a && b` (or `! a`) by its last pipeline.
  Decompiler d{{prog, pc}};
  for (;;) {
    WordCode  w   = d.fetch();  // WC_SUBLIST
    s::size_t end = d.pc + wc_sublist_skip(w);
    if ((wc_sublist_flags(w) & WC_SUBLIST_TYPE) == WC_SUBLIST_END or end >= prog.prog.size())
      return d.pipeline();
    d.pc = end;
  }
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

// A function's name as zsh prints it: in single quotes when it has
// characters a word would not keep (`'a b' () {`).
static str name_text(strv name) {
  bool plain = not name.empty() and s::all_of(name.begin(), name.end(), [](char c) {
    return s::isalnum(static_cast<unsigned char>(c)) or strv("_-./+:@%,").find(c) != strv::npos
        or static_cast<unsigned char>(c) >= 0x80;
  });
  return plain ? str(name) : single_quote(name);
}

str program_text(const CompiledProgram& prog) {
  Pretty pr{{prog, 0}};
  pr.body(0);
  pr.flush_docs();
  return pr.out;
}

str list_text(const CompiledProgram& prog, s::size_t pc) {
  Pretty pr{{prog, pc}};
  pr.sublist(0);
  pr.flush_docs();
  return pr.out;
}

str function_text(strv names, const CompiledProgram& body) {
  str head = name_text(names);
  if (redirected_body(body)) {
    Pretty pr{{body, 0}};
    pr.out = head + " () ";
    pr.body(0);
    pr.flush_docs();
    return pr.out;
  }
  Pretty pr{{body, 0}};
  pr.out = head + " () {\n\t";
  pr.body(1);
  pr.flush_docs();
  pr.out += "\n}";
  return pr.out;
}

}  // namespace jfsh
