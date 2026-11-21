#include <jfsh/parser/parser.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/lexer/input.h>

namespace jfsh {

static bool is_reserved_word(TokType t) noexcept;

Result<ProgRef> Parser::parse() {
  auto prog = s::make_shared<CompiledProgram>();
  mProg  = prog.get();
  mError = {};
  mIncomplete = false;
  bool ok = par_list();
  if (ok and not at(TokType::TK_EOF)) ok = error_near(mLex.peek());
  if (not ok and mError.message.empty()) error_near(mLex.peek());
  if (not ok) {
    mProg = nullptr;
    mPendingHeredocs.clear();
    return s::unexpected(mError);
  }
  auto bodies = mLex.take_heredocs();
  for (s::size_t i = 0; i < mPendingHeredocs.size() and i < bodies.size(); ++i)
    prog->strs[mPendingHeredocs[i]] = s::move(bodies[i]);
  mPendingHeredocs.clear();
  emit(make_wc(WC_END, 0));
  mProg = nullptr;
  return prog;
}

Result<ProgRef> Parser::parse_line() {
  auto prog = s::make_shared<CompiledProgram>();
  mProg  = prog.get();
  mError = {};
  mIncomplete = false;
  mLineMode = true;
  // A line of only `;` is an empty command, as in zsh.
  while (eat(TokType::TK_NEWLINE) or eat(TokType::TK_SEMI)) {}
  bool ok = at(TokType::TK_EOF) or par_list1();
  mLineMode = false;
  if (ok and not at(TokType::TK_EOF) and not at(TokType::TK_NEWLINE))
    ok = error_near(mLex.peek());
  if (not ok and mError.message.empty()) error_near(mLex.peek());
  if (not ok) {
    mProg = nullptr;
    mPendingHeredocs.clear();
    return s::unexpected(mError);
  }
  // The newline that ended the line reads the here-document bodies.
  if (at(TokType::TK_NEWLINE)) mLex.consume();
  auto bodies = mLex.take_heredocs();
  for (s::size_t i = 0; i < mPendingHeredocs.size() and i < bodies.size(); ++i)
    prog->strs[mPendingHeredocs[i]] = s::move(bodies[i]);
  mPendingHeredocs.clear();
  emit(make_wc(WC_END, 0));
  mProg = nullptr;
  return prog;
}

bool Parser::expand_alias(bool cmdpos) {
  if (not mAliases or not mLex.alias_expansion()) return false;
  const LexToken& tok = mLex.peek();
  if (not (tok.type == TokType::TK_WORD or is_reserved_word(tok.type)
           or tok.type == TokType::TK_LBRACE or tok.type == TokType::TK_RBRACE))
    return false;
  const str& name = tok.text;
  if (name.empty()) return false;
  auto in_use = [&](const str& n) {
    return tok.aliases and s::find(tok.aliases->begin(), tok.aliases->end(), n)
                           != tok.aliases->end();
  };
  str key;
  str text;
  if (auto it = mAliases->regular.find(name);
      it != mAliases->regular.end() and (cmdpos or it->second.global)
      and not in_use(name)) {
    key  = name;
    text = it->second.text;
  } else if (cmdpos and not mAliases->suffix.empty()) {
    // `file.ext` runs as `text file.ext`.
    s::size_t slash = name.rfind('/');
    s::size_t dot   = name.rfind('.');
    if (dot == str::npos or dot == 0 or (slash != str::npos and dot < slash + 2)
        or dot + 1 == name.size())
      return false;
    auto sfx = mAliases->suffix.find(name.substr(dot + 1));
    if (sfx == mAliases->suffix.end() or in_use("." + sfx->first)) return false;
    key  = "." + sfx->first;
    text = sfx->second + " " + name;
  } else {
    return false;
  }
  auto names = s::make_shared<vec<str>>(tok.aliases ? *tok.aliases : vec<str>{});
  names->push_back(s::move(key));
  // A space after the text keeps the alias's last word apart from what
  // follows it, as zsh's input stack does.
  bool blank_end = not text.empty() and (text.back() == ' ' or text.back() == '\t');
  mLex.consume();
  mLex.push_alias(blank_end ? text : text + " ", s::move(names));
  return true;
}

bool is_incomplete(strv src, bool comments) {
  StringInputSource in{str(src)};
  Lexer lex{in};
  if (comments) lex.push_state(LEX_SKIPCOMMENTS);
  Parser parser{lex};
  auto prog = parser.parse();
  return lex.incomplete() or (not prog and parser.incomplete());
}

Result<ProgRef> Parser::parse_string(strv src) {
  StringInputSource in{str(src)};
  Lexer lex{in};
  lex.push_state(LEX_SKIPCOMMENTS);
  Parser p{lex, mFlags};
  return p.parse();
}

s::size_t Parser::emit(WordCode w) {
  s::size_t pos = mProg->prog.size();
  mProg->prog.push_back(w);
  mProg->line_map.push_back(static_cast<s::uint32_t>(mLex.line()));
  return pos;
}

s::size_t Parser::emit_str(str s) {
  s::size_t idx = mProg->strs.size();
  mProg->strs.push_back(s::move(s));
  emit(make_wc(WC_STRING, static_cast<WordCode>(idx)));
  return idx;
}

// Words emitted since the instruction at addr.
static WordCode words_after(const CompiledProgram& prog, s::size_t addr) {
  return static_cast<WordCode>(prog.prog.size() - addr - 1);
}

// Reserved words are only reserved in command position; anywhere else
// (`echo done`, `git log --until …`) they are ordinary words.
static bool is_reserved_word(TokType t) noexcept {
  return (t >= TokType::TK_IF and t <= TokType::TK_ALWAYS)
      or t == TokType::TK_BANG;
}

bool Parser::at_word() const noexcept {
  return at(TokType::TK_WORD) or is_reserved_word(mLex.peek().type);
}

bool Parser::expect(TokType t) {
  if (eat(t)) return true;
  return error_near(mLex.peek());
}

// `<(…)`, `>(…)` or `=(…)`: process substitution, which the lexer returns
// as a redirection token holding the whole command.
static bool is_procsubst(strv text) noexcept {
  return text.size() >= 2 and text.back() == ')'
     and (text.starts_with("<(") or text.starts_with(">(")
          or text.starts_with("=("));
}

bool Parser::at_list_word() const noexcept {
  return at_word() or (at(TokType::TK_REDIR) and is_procsubst(mLex.peek().text));
}

// Tokens that end a list: end of input, or the closing word of whatever
// compound command the list is the body of.
bool Parser::at_list_terminator() const noexcept {
  switch (mLex.peek().type) {
    case TokType::TK_EOF:
    case TokType::TK_RBRACE:  case TokType::TK_RPAREN:
    case TokType::TK_THEN:    case TokType::TK_ELIF:
    case TokType::TK_ELSE:    case TokType::TK_FI:
    case TokType::TK_DO:      case TokType::TK_DONE:
    case TokType::TK_ESAC:    case TokType::TK_DSEMI:
    case TokType::TK_END:
    case TokType::TK_SEMIAMP: case TokType::TK_SEMIBAR:
      return true;
    default:
      return false;
  }
}

bool Parser::error_near(const LexToken& tok) {
  mIncomplete = tok.is_eof();
  if (tok.is_eof())          error("parse error: unexpected end of input");
  else if (tok.is_newline()) error("parse error near `\\n'");
  else                       error("parse error near `" + tok.text + "'");
  return false;
}

// An empty list is allowed (`f() { }`); at the top level the token that
// stopped it is then reported by parse().
bool Parser::par_list() {
  // `then; cmd`, `do; done`: separators may start a list.
  while (eat(TokType::TK_NEWLINE) or eat(TokType::TK_SEMI)) {}
  if (at_list_terminator()) return true;
  return par_list1();
}

bool Parser::par_body() {
  if (not par_list()) return false;
  emit(make_wc(WC_END, 0));
  return true;
}

void Parser::emit_words(vec<str> words) {
  emit(make_wc(WC_WORDS, static_cast<WordCode>(words.size())));
  for (auto& w : words) emit_str(s::move(w));
}

// list := WC_LIST(flags, skip) sublist, repeated for each `;`, `&` or
// newline-separated element.
bool Parser::par_list1() {
  // parse_line stops at a newline outside any construct.
  bool top = mLineMode;
  mLineMode = false;
  for (;;) {
    s::size_t head = emit(make_wc_list(0, 0));
    if (not par_sublist()) return false;
    WordCode flags = 0;
    bool     more  = false;
    if (top and at(TokType::TK_NEWLINE)) {
      patch(head, make_wc_list(flags | WC_LIST_END, words_after(*mProg, head)));
      return true;
    }
    if (at(TokType::TK_SEMI) or at(TokType::TK_AMP) or at(TokType::TK_NEWLINE)) {
      if (at(TokType::TK_AMP)) {
        flags |= WC_LIST_ASYNC;
        if (mLex.peek().text != "&") flags |= WC_LIST_DISOWN;
      }
      mLex.consume();
      // `cmd & ;` and `a; ; b` are allowed, as in zsh: an empty command.
      while (eat(TokType::TK_SEMI)) {}
      if (top and at(TokType::TK_NEWLINE)) {
        patch(head, make_wc_list(flags | WC_LIST_END, words_after(*mProg, head)));
        return true;
      }
      while (eat(TokType::TK_NEWLINE) or eat(TokType::TK_SEMI)) {}
      more = not at_list_terminator();
    }
    if (not more) flags |= WC_LIST_END;
    patch(head, make_wc_list(flags, words_after(*mProg, head)));
    if (not more) return true;
  }
}

// sublist := WC_SUBLIST(connector | NOT, skip) pipeline, chained by && / ||.
bool Parser::par_sublist() {
  for (;;) {
    s::size_t head = emit(make_wc_sublist(0, 0));
    WordCode flags = eat(TokType::TK_BANG) ? WC_SUBLIST_NOT : 0;
    if (not par_pline()) return false;
    WordCode skip = words_after(*mProg, head);
    if      (at(TokType::TK_AMPAMP))   flags |= WC_SUBLIST_AND;
    else if (at(TokType::TK_PIPEPIPE)) flags |= WC_SUBLIST_OR;
    patch(head, make_wc_sublist(flags, skip));
    if ((flags & WC_SUBLIST_TYPE) == WC_SUBLIST_END) return true;
    mLex.consume();
    skip_newlines();
  }
}

// pipeline := WC_PIPE(count|stderr_mask) command{n}
bool Parser::par_pline() {
  s::size_t head = emit(make_wc_pipe(0, 0));
  WordCode n           = 0;
  WordCode stderr_mask = 0;
  for (;;) {
    if (not par_cmd()) return false;
    ++n;
    if (not (at(TokType::TK_PIPE) or at(TokType::TK_PIPEAMP))) break;
    if (at(TokType::TK_PIPEAMP)) stderr_mask |= (1u << (n - 1));
    mLex.consume();
    skip_newlines();
  }
  patch(head, make_wc_pipe(n, stderr_mask));
  return true;
}

bool Parser::par_cmd() {
  skip_newlines();
  while (expand_alias(true)) skip_newlines();
  s::size_t start = mProg->prog.size();
  // A command's words are emitted after the lexer has read past them, so
  // its first word is tagged with the line the command starts on ($LINENO).
  auto first_line = static_cast<s::uint32_t>(mLex.peek().line);
  auto tag_line = [&](bool parsed) {
    if (parsed and start < mProg->line_map.size()) mProg->line_map[start] = first_line;
    return parsed;
  };
  bool ok = false;
  switch (mLex.peek().type) {
    case TokType::TK_IF:       ok = par_if();      break;
    case TokType::TK_WHILE:
    case TokType::TK_UNTIL:    ok = par_while();   break;
    case TokType::TK_FOR:      ok = par_for();     break;
    case TokType::TK_FOREACH:  ok = par_foreach(); break;
    case TokType::TK_CASE:     ok = par_case();    break;
    case TokType::TK_SELECT:   ok = par_select();  break;
    case TokType::TK_REPEAT:   ok = par_repeat();  break;
    case TokType::TK_LPAREN:   ok = par_subsh();   break;
    case TokType::TK_LBRACE:   ok = par_cursh();   break;
    case TokType::TK_FUNCTION: ok = par_funcdef(); break;
    case TokType::TK_DLPAREN:  ok = par_arith();   break;
    case TokType::TK_WORD:
      // `{cmd …}`: in command position a `{` opens a group even with the
      // word stuck to it, as in zsh.
      if (split_open_brace()) {
        ok = par_cursh(true);
        break;
      }
      if (mLex.peek().text != "[[") return tag_line(par_simple());
      ok = par_cond();
      break; default: return tag_line(par_simple());
  }
  return tag_line(ok and par_trailing_redirs(start));
}

bool Parser::par_trailing_redirs(s::size_t start) {
  s::size_t mid = mProg->prog.size();
  while (at(TokType::TK_REDIR)) {
    if (is_procsubst(mLex.peek().text)) return error_near(mLex.peek());
    if (not par_redir()) return false;
  }
  auto& prog = mProg->prog;
  auto& lines = mProg->line_map;
  auto from = static_cast<s::ptrdiff_t>(start);
  auto to   = static_cast<s::ptrdiff_t>(mid);
  s::rotate(prog.begin() + from, prog.begin() + to, prog.end());
  s::rotate(lines.begin() + from, lines.begin() + to, lines.end());
  return true;
}

// `NAME=value` or `NAME+=value` with an unquoted identifier before the `=`.
// Returns the length of NAME, or 0 when the word is not an assignment.
static s::size_t assignment_name_len(strv w, bool& append) {
  auto ident_start = [](char c) {
    return s::isalpha(static_cast<unsigned char>(c)) or c == '_';
  };
  auto ident_char = [](char c) {
    return s::isalnum(static_cast<unsigned char>(c)) or c == '_';
  };
  if (w.empty()) return 0;
  s::size_t i = 1;
  if (s::isdigit(static_cast<unsigned char>(w[0]))) {
    // `2=value`: a positional parameter.
    while (i < w.size() and s::isdigit(static_cast<unsigned char>(w[i]))) ++i;
    append = i + 1 < w.size() and w[i] == '+' and w[i + 1] == '=';
    if (append) return i;
    return i < w.size() and w[i] == '=' ? i : 0;
  }
  if (not ident_start(w[0])) return 0;
  while (i < w.size() and ident_char(w[i])) ++i;
  // `NAME[subscript]=value`: one element of an array or association.
  if (i < w.size() and w[i] == '[') {
    int depth = 0;
    s::size_t k = i;
    for (; k < w.size(); ++k) {
      if (w[k] == '\\' and k + 1 < w.size()) { ++k; continue; }
      if (w[k] == '[') ++depth;
      else if (w[k] == ']' and --depth == 0) break;
    }
    if (k >= w.size()) return 0;
    i = k + 1;
  }
  append = false;
  if (i + 1 < w.size() and w[i] == '+' and w[i + 1] == '=') {
    append = true;
    return i;
  }
  return (i < w.size() and w[i] == '=') ? i : 0;
}

// Commands whose `name=value` arguments are assignments, expanded without
// splitting (`local x=$(cmd)`), as zsh's reserved-word typeset does.
static bool is_typeset_command(strv w) noexcept {
  return w == "typeset" or w == "local" or w == "declare" or w == "export"
      or w == "readonly" or w == "integer" or w == "float" or w == "private";
}

// An assignment word after its NAME (of length len): sets the WC_ASSIGN
// flags and the value, without the parentheses of an array.
static str assignment_value(strv word, s::size_t len,
                                    bool append, WordCode& flags) {
  str value(word.substr(len + (append ? 2 : 1)));
  flags = append ? WC_ASSIGN_APPEND : 0;
  if (value.size() >= 2 and value.front() == '(' and value.back() == ')') {
    flags |= WC_ASSIGN_ARRAY;
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

// simple := (assign | redir)* WC_SIMPLE(argc) WC_STRING{argc}
//         | (assign | redir)* WC_TYPESET(n) item{n}
// A word list followed by `()` is a function definition instead.
bool Parser::par_simple() {
  struct Item {
    str word;      // the name, for an assignment
    str value  = {};
    WordCode    flags  = 0;
    bool        assign = false;
  };
  vec<str>  words;
  vec<Item> items;   // typeset arguments, in order
  bool typeset = false;
  int  nassign = 0;
  int  nredir  = 0;
  for (;;) {
    // After the command word `(` starts a glob group word.
    bool argpos = not words.empty() and not typeset;
    if (argpos) mLex.push_state(static_cast<LexFlags>(mLex.state() | LEX_ARGPAREN));
    const auto& tok = mLex.peek();
    if (argpos) mLex.pop_state();
    if (tok.type == TokType::TK_REDIR) {
      // Process substitution reads like a redirection to the lexer, but it
      // behaves like a WORD whose value is `/dev/fd/N` once the pipe is up.
      // Forward it through as an argument; the executor sets up the pipe
      // during word expansion and substitutes the path in-place.
      if (is_procsubst(tok.text)) {
        words.push_back(tok.text);
        if (typeset) items.push_back({tok.text});
        mLex.consume();
        continue;
      }
      if (not par_redir()) return false;
      ++nredir;
      continue;
    }
    // Aliases: the command word (after any assignments), a word after an
    // alias ending in a blank, and global aliases anywhere.
    bool cmdpos = words.empty() or (tok.after_alias_blank and not typeset);
    if (expand_alias(cmdpos)) continue;
    if (tok.type == TokType::TK_WORD) {
      bool append = false;
      s::size_t len = (words.empty() or typeset)
                    ? assignment_name_len(tok.text, append) : 0;
      if (len) {
        WordCode flags = 0;
        str value = assignment_value(tok.text, len, append, flags);
        if (typeset) {
          items.push_back({tok.text.substr(0, len), s::move(value), flags, true});
        } else {
          emit(make_wc(WC_ASSIGN, flags));
          emit_str(tok.text.substr(0, len));
          emit_str(s::move(value));
          ++nassign;
        }
      } else {
        if (words.empty() and is_typeset_command(tok.text)) typeset = true;
        words.push_back(tok.text);
        if (typeset) items.push_back({tok.text});
      }
      mLex.consume();
      continue;
    }
    // After the command word a reserved word, or `{`, is an argument
    // (`echo done {`); `}` still ends a brace group.
    if (not words.empty()
        and (is_reserved_word(tok.type) or tok.type == TokType::TK_LBRACE)) {
      words.push_back(tok.text);
      if (typeset) items.push_back({tok.text});
      mLex.consume();
      continue;
    }
    break;
  }
  if (at(TokType::TK_LPAREN) and not words.empty() and not typeset
      and nassign == 0 and nredir == 0) {
    mLex.consume();
    if (not expect(TokType::TK_RPAREN)) return false;
    return par_funcdef_body(s::move(words));
  }
  if (words.empty() and nassign == 0 and nredir == 0)
    return error_near(mLex.peek());
  if (typeset) {
    emit(make_wc(WC_TYPESET, static_cast<WordCode>(items.size())));
    for (auto& item : items) {
      if (item.assign) {
        emit(make_wc(WC_ASSIGN, item.flags));
        emit_str(s::move(item.word));
        emit_str(s::move(item.value));
      } else {
        emit_str(s::move(item.word));
      }
    }
    return true;
  }
  emit(make_wc(WC_SIMPLE, static_cast<WordCode>(words.size())));
  for (auto& w : words) emit_str(s::move(w));
  return true;
}

// if := WC_IF(HEAD, skip) branch+, one branch for `if`, each `elif` and
// the `else`.  Besides `then … fi`, a branch may be zsh's short form
// `cond { list }`, after which `elif cond { … }` / `else { … }` follow
// and no `fi` closes it, or a single command after the condition
// (`if [[ -f x ]] print found`).
bool Parser::par_if() {
  mLex.consume(); // if
  s::size_t head  = emit(make_wc_cmd(WC_IF, WC_IF_HEAD, 0));
  auto braced = [&] {
    return par_body() and expect(TokType::TK_RBRACE);
  };
  for (;;) {
    s::size_t branch = emit(make_wc_cmd(WC_IF, WC_IF_IF, 0));
    if (not par_body()) return false;  // condition
    bool shortform = true;
    if (eat(TokType::TK_THEN)) {
      shortform = false;
      if (not par_body()) return false;
    } else if (eat(TokType::TK_LBRACE)) {
      if (not braced()) return false;
    } else if (at_word() and not at_list_terminator()) {
      if (not par_sublist_body()) return false;
      patch(branch, make_wc_cmd(WC_IF, WC_IF_IF, words_after(*mProg, branch)));
      break;
    } else {
      return error_near(mLex.peek());
    }
    patch(branch, make_wc_cmd(WC_IF, WC_IF_IF, words_after(*mProg, branch)));
    if (eat(TokType::TK_ELIF)) continue;
    if (eat(TokType::TK_ELSE)) {
      s::size_t other = emit(make_wc_cmd(WC_IF, WC_IF_ELSE, 0));
      if (shortform) {
        skip_newlines();
        if (not expect(TokType::TK_LBRACE) or not braced()) return false;
      } else if (not par_body()) {
        return false;
      }
      patch(other, make_wc_cmd(WC_IF, WC_IF_ELSE, words_after(*mProg, other)));
    }
    if (not shortform and not expect(TokType::TK_FI)) return false;
    break;
  }
  patch(head, make_wc_cmd(WC_IF, WC_IF_HEAD, words_after(*mProg, head)));
  return true;
}

bool Parser::par_loop_body() {
  skip_newlines();
  if (eat(TokType::TK_DO))
    return par_body() and expect(TokType::TK_DONE);
  if (eat(TokType::TK_LBRACE))
    return par_body() and expect(TokType::TK_RBRACE);
  return par_sublist_body();
}

bool Parser::par_sublist_body() {
  s::size_t list = emit(make_wc_list(0, 0));
  if (not par_sublist()) return false;
  patch(list, make_wc_list(WC_LIST_END, words_after(*mProg, list)));
  emit(make_wc(WC_END, 0));
  return true;
}

// while := WC_WHILE(WHILE|UNTIL, skip) body(cond) body
bool Parser::par_while() {
  WordCode type = at(TokType::TK_UNTIL) ? WC_WHILE_UNTIL : WC_WHILE_WHILE;
  mLex.consume();
  s::size_t head = emit(make_wc_cmd(WC_WHILE, type, 0));
  if (not par_body()) return false;
  if (not par_loop_body()) return false;
  patch(head, make_wc_cmd(WC_WHILE, type, words_after(*mProg, head)));
  return true;
}

// The rest of `for`/`select` after the keyword (head already emitted):
//   names in words ;    names ( words )    names [;]   (loops over "$@")
bool Parser::par_for_words(s::size_t head, WordCode op) {
  vec<str> names;
  while (at(TokType::TK_WORD)) {
    names.push_back(mLex.peek().text);
    mLex.consume();
  }
  if (names.empty()) return error_near(mLex.peek());
  skip_newlines();
  WordCode type = WC_FOR_IN;
  vec<str> values;
  if (eat(TokType::TK_IN)) {
    // The words are arguments: `(` starts a glob group word.
    auto word_ahead = [&] {
      mLex.push_state(static_cast<LexFlags>(mLex.state() | LEX_ARGPAREN));
      bool ok = at_list_word();
      mLex.pop_state();
      return ok;
    };
    while (word_ahead()) {
      values.push_back(mLex.peek().text);
      mLex.consume();
    }
    if (not (eat(TokType::TK_SEMI) or eat(TokType::TK_NEWLINE)))
      return error_near(mLex.peek());
  } else if (eat(TokType::TK_LPAREN)) {
    for (skip_newlines(); not eat(TokType::TK_RPAREN); skip_newlines()) {
      if (not at_list_word()) return error_near(mLex.peek());
      values.push_back(mLex.peek().text);
      mLex.consume();
    }
    eat(TokType::TK_SEMI);
  } else {
    type = WC_FOR_ARGS;
    eat(TokType::TK_SEMI);
  }
  emit_words(s::move(names));
  if (type == WC_FOR_IN) emit_words(s::move(values));
  if (not par_loop_body()) return false;
  patch(head, make_wc_cmd(op, type, words_after(*mProg, head)));
  return true;
}

// foreach := `foreach name… ( words ) list end` or `foreach name… in
// words ; list end`, the csh-style loop (or with a `do`/`{` body);
// compiled as WC_FOR(IN).
bool Parser::par_foreach() {
  mLex.consume();  // foreach
  s::size_t head = emit(make_wc_cmd(WC_FOR, WC_FOR_IN, 0));
  vec<str> names;
  while (at(TokType::TK_WORD)) {
    names.push_back(mLex.peek().text);
    mLex.consume();
  }
  if (names.empty()) return error_near(mLex.peek());
  vec<str> values;
  if (eat(TokType::TK_IN)) {
    while (at_list_word() and not at(TokType::TK_END)) {
      values.push_back(mLex.peek().text);
      mLex.consume();
    }
  } else if (eat(TokType::TK_LPAREN)) {
    for (skip_newlines(); not eat(TokType::TK_RPAREN); skip_newlines()) {
      if (not at_list_word()) return error_near(mLex.peek());
      values.push_back(mLex.peek().text);
      mLex.consume();
    }
  } else {
    return error_near(mLex.peek());
  }
  eat(TokType::TK_SEMI);
  emit_words(s::move(names));
  emit_words(s::move(values));
  // As in zsh's par_for, `do … done` and `{ … }` work here too.
  skip_newlines();
  if (at(TokType::TK_DO) or at(TokType::TK_LBRACE)) {
    if (not par_loop_body()) return false;
  } else if (not par_body() or not expect(TokType::TK_END)) {
    return false;
  }
  patch(head, make_wc_cmd(WC_FOR, WC_FOR_IN, words_after(*mProg, head)));
  return true;
}

// Splits the text of `for (( init; cond; step ))` at its top-level `;`s.
static bool split_for_cond(strv text, vec<str>& out) {
  int depth = 0;
  s::size_t start = 0;
  for (s::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '(') ++depth;
    else if (text[i] == ')') --depth;
    else if (text[i] == ';' and depth == 0) {
      out.emplace_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  out.emplace_back(text.substr(start));
  return out.size() == 3;
}

// for  := WC_FOR(IN|ARGS, skip) words(names) [words(values)] body
// cfor := WC_FOR_COND(0, skip) WC_STRING{3} body
bool Parser::par_for() {
  mLex.consume();  // for
  if (at(TokType::TK_DLPAREN)) {
    mLex.consume();
    auto text = mLex.read_arith();
    if (not text) return error_near(mLex.peek());
    vec<str> parts;
    if (not split_for_cond(*text, parts)) {
      error("parse error near `((" + *text + "))'");
      return false;
    }
    s::size_t head = emit(make_wc_cmd(WC_FOR_COND, 0, 0));
    for (auto& part : parts) emit_str(s::move(part));
    eat(TokType::TK_SEMI);
    if (not par_loop_body()) return false;
    patch(head, make_wc_cmd(WC_FOR_COND, 0, words_after(*mProg, head)));
    return true;
  }
  s::size_t head = emit(make_wc_cmd(WC_FOR, 0, 0));
  return par_for_words(head, WC_FOR);
}

// case := WC_CASE(HEAD, skip) WC_STRING(subject) arm*
// arm  := WC_CASE(BREAK|FALL|TESTNEXT, skip) words(patterns) body
// `(a | b)` read whole as a case pattern and followed by the commands:
// the alternatives inside, blanks around them dropped; null unless the
// parentheses enclose the whole word.
static opt<vec<str>> unparen_case_pattern(strv w) {
  vec<str> out;
  str cur;
  int depth = 0;
  for (s::size_t i = 1; i + 1 < w.size(); ++i) {
    char c = w[i];
    if (c == '\\' and i + 2 < w.size()) {
      cur += c;
      cur += w[++i];
      continue;
    }
    if (c == '\'' or c == '"') {
      s::size_t e = i + 1;
      while (e + 1 < w.size() and w[e] != c) e += (c == '"' and w[e] == '\\') ? 2 : 1;
      cur += w.substr(i, e - i + 1);
      i = e;
      continue;
    }
    if (c == '(') ++depth;
    if (c == ')' and --depth < 0) return s::nullopt;
    if (c == '|' and depth == 0) {
      out.push_back(s::move(cur));
      cur.clear();
      continue;
    }
    cur += c;
  }
  if (depth != 0) return s::nullopt;
  out.push_back(s::move(cur));
  for (auto& p : out) {
    while (not p.empty() and (p.front() == ' ' or p.front() == '\t')) p.erase(0, 1);
    while (not p.empty() and (p.back() == ' ' or p.back() == '\t')) p.pop_back();
  }
  return out;
}

bool Parser::par_case() {
  mLex.consume();  // case
  // `case (word) in`: the parentheses are part of the word.
  mLex.push_state(static_cast<LexFlags>(mLex.state() | LEX_CASEPAT));
  bool word = at_list_word();
  mLex.pop_state();
  if (not word) return error_near(mLex.peek());
  str subject = mLex.peek().text;
  mLex.consume();
  // `case word; in` is allowed too, and `in;`.
  while (eat(TokType::TK_NEWLINE) or eat(TokType::TK_SEMI)) {}
  if (not expect(TokType::TK_IN)) return false;
  s::size_t head = emit(make_wc_cmd(WC_CASE, WC_CASE_HEAD, 0));
  emit_str(s::move(subject));
  for (;;) {
    // The patterns are read with a leading `(…)` as part of a word:
    // `(a|b))` is the group as the pattern; `(a|b) cmd` the optional
    // parenthesis around a|b, as zsh's par_case decides.
    mLex.push_state(static_cast<LexFlags>(mLex.state() | LEX_CASEPAT));
    struct PopState {
      ~PopState() { lex.pop_state(); }
      Lexer& lex;
    };
    opt<PopState> casepat;
    casepat.emplace(mLex);
    while (eat(TokType::TK_NEWLINE) or eat(TokType::TK_SEMI)) {}
    if (eat(TokType::TK_ESAC)) break;
    s::size_t arm = emit(make_wc_cmd(WC_CASE, WC_CASE_BREAK, 0));
    eat(TokType::TK_LPAREN);
    vec<str> patterns;
    bool closed = false;
    for (;;) {
      // An empty alternative (`|a|b)`, `a|)`) matches the empty string.
      if (at(TokType::TK_PIPE)) {
        patterns.emplace_back();
        mLex.consume();
        continue;
      }
      if (not at_word() and not patterns.empty() and at(TokType::TK_RPAREN)) {
        patterns.emplace_back();
        break;
      }
      if (not at_word()) return error_near(mLex.peek());
      str w = mLex.peek().text;
      mLex.consume();
      if (patterns.empty() and w.size() >= 2 and w[0] == '(' and w.back() == ')'
          and not at(TokType::TK_RPAREN) and not at(TokType::TK_PIPE)) {
        auto inner = unparen_case_pattern(w);
        if (not inner) return error_near(mLex.peek());
        patterns = s::move(*inner);
        closed = true;
        break;
      }
      patterns.push_back(s::move(w));
      if (not eat(TokType::TK_PIPE)) break;
    }
    if (not closed and not expect(TokType::TK_RPAREN)) return false;
    casepat.reset();
    // `(a) (cmd);;`: the body's `(` was peeked as a pattern.
    if (closed) mLex.relex_peeked();
    emit_words(s::move(patterns));
    if (not par_body()) return false;
    WordCode type = WC_CASE_BREAK;
    if      (eat(TokType::TK_SEMIAMP)) type = WC_CASE_FALL;
    else if (eat(TokType::TK_SEMIBAR)) type = WC_CASE_TESTNEXT;
    else if (not eat(TokType::TK_DSEMI) and not at(TokType::TK_ESAC))
      return error_near(mLex.peek());
    patch(arm, make_wc_cmd(WC_CASE, type, words_after(*mProg, arm)));
  }
  patch(head, make_wc_cmd(WC_CASE, WC_CASE_HEAD, words_after(*mProg, head)));
  return true;
}

// select := WC_SELECT(IN|ARGS, skip), laid out like for.
bool Parser::par_select() {
  mLex.consume(); // select
  s::size_t head  = emit(make_wc_cmd(WC_SELECT, 0, 0));
  return par_for_words(head, WC_SELECT);
}

// repeat := WC_REPEAT(0, skip) WC_STRING(count) body
bool Parser::par_repeat() {
  mLex.consume();  // repeat
  if (not at_word()) return error_near(mLex.peek());
  s::size_t head = emit(make_wc_cmd(WC_REPEAT, 0, 0));
  emit_str(mLex.peek().text);
  mLex.consume();
  eat(TokType::TK_SEMI);
  if (not par_loop_body()) return false;
  patch(head, make_wc_cmd(WC_REPEAT, 0, words_after(*mProg, head)));
  return true;
}

// subsh := WC_SUBSH(0, skip) body, or `() { … } args`, an anonymous
// function called at once: WC_FUNCDEF(ANON, skip) body words(args).
bool Parser::par_subsh() {
  mLex.consume();  // (
  if (eat(TokType::TK_RPAREN)) return par_anon_function();
  s::size_t head = emit(make_wc_cmd(WC_SUBSH, 0, 0));
  if (not par_body() or not expect(TokType::TK_RPAREN)) return false;
  patch(head, make_wc_cmd(WC_SUBSH, 0, words_after(*mProg, head)));
  return true;
}

// cursh := WC_CURSH(0, skip) body
// try   := WC_TRY(0, skip) body body     `{ … } always { … }`
bool Parser::par_cursh(bool opened) {
  if (not opened) mLex.consume(); // {
  s::size_t head  = emit(make_wc_cmd(WC_CURSH, 0, 0));
  if (not par_body() or not expect(TokType::TK_RBRACE)) return false;
  WordCode op = WC_CURSH;
  if (eat(TokType::TK_ALWAYS)) {
    op = WC_TRY;
    if (not expect(TokType::TK_LBRACE)) return false;
    if (not par_body() or not expect(TokType::TK_RBRACE)) return false;
  }
  patch(head, make_wc_cmd(op, 0, words_after(*mProg, head)));
  return true;
}

// `function name… [()] body`; `name… () body` comes through par_simple.
// `() { … } args` or `function { … } args`, called at once:
// WC_FUNCDEF(ANON, skip) body words(args).
bool Parser::par_anon_function() {
  s::size_t head = emit(make_wc_cmd(WC_FUNCDEF, WC_FUNCDEF_ANON, 0));
  if (not par_function_body()) return false;
  vec<str> args;
  while (at_list_word()) {
    args.push_back(mLex.peek().text);
    mLex.consume();
  }
  emit_words(s::move(args));
  patch(head, make_wc_cmd(WC_FUNCDEF, WC_FUNCDEF_ANON, words_after(*mProg, head)));
  return true;
}

bool Parser::par_funcdef() {
  mLex.consume();  // function
  vec<str> names;
  while (at(TokType::TK_WORD) and mLex.peek().text != "{}") {
    names.push_back(mLex.peek().text);
    mLex.consume();
  }
  if (names.empty()) {
    // `function { … }`, `function () { … }`: anonymous.
    if (eat(TokType::TK_LPAREN) and not expect(TokType::TK_RPAREN)) return false;
    return par_anon_function();
  }
  if (eat(TokType::TK_LPAREN) and not expect(TokType::TK_RPAREN)) return false;
  return par_funcdef_body(s::move(names));
}

// funcdef := WC_FUNCDEF(NAMED, skip) words(names) body
bool Parser::par_funcdef_body(vec<str> names) {
  s::size_t head = emit(make_wc_cmd(WC_FUNCDEF, WC_FUNCDEF_NAMED, 0));
  emit_words(s::move(names));
  if (not par_function_body()) return false;
  patch(head, make_wc_cmd(WC_FUNCDEF, WC_FUNCDEF_NAMED, words_after(*mProg, head)));
  return true;
}

// A word `{text` at the command's start: put back as `{` and the word
// `text`.  False (nothing changed) for any other token.
bool Parser::split_open_brace() {
  const LexToken& t = mLex.peek();
  if (t.type != TokType::TK_WORD or t.text.size() < 2 or t.text[0] != '{' or t.text == "{}")
    return false;
  // The rest is read again as tokens, so `{echo}`'s `}` closes the group.
  str rest = t.text.substr(1);
  mLex.consume();
  mLex.push_back_text(s::move(rest));
  return true;
}

bool Parser::par_function_body() {
  skip_newlines();
  if (split_open_brace()) {
    s::size_t start = mProg->prog.size();
    return par_body() and expect(TokType::TK_RBRACE) and wrap_body_redirs(start);
  }
  // `f() {}`: the lexer reads `{}` as one word; it is an empty body.
  if (at(TokType::TK_WORD) and mLex.peek().text == "{}") {
    mLex.consume();
    s::size_t start = mProg->prog.size();
    emit(make_wc(WC_END, 0));
    return wrap_body_redirs(start);
  }
  if (eat(TokType::TK_LBRACE)) {
    s::size_t start = mProg->prog.size();
    return par_body() and expect(TokType::TK_RBRACE) and wrap_body_redirs(start);
  }
  return par_sublist_body();
}

// `f() { … } > log`: redirections after the body belong to the function
// and apply on every call.  The body from `start` becomes the one command
// `{ … } > log` of a new body: list sublist pipe redirs WC_CURSH body END.
bool Parser::wrap_body_redirs(s::size_t start) {
  // A process substitution there is an argument of an anonymous
  // function: `() { … } =(cmd)`.
  if (not at(TokType::TK_REDIR) or is_procsubst(mLex.peek().text)) return true;
  insert(start, make_wc_cmd(WC_CURSH, 0, 0));
  patch(start, make_wc_cmd(WC_CURSH, 0, words_after(*mProg, start)));
  if (not par_trailing_redirs(start)) return false;
  insert(start, make_wc_pipe(1, 0));
  insert(start, make_wc_sublist(0, 0));
  patch(start, make_wc_sublist(0, words_after(*mProg, start)));
  insert(start, make_wc_list(0, 0));
  patch(start, make_wc_list(WC_LIST_END, words_after(*mProg, start)));
  emit(make_wc(WC_END, 0));
  return true;
}

// `[[ cond ]]`, read with the lexer in LEX_COND state, so `<` and `>` are
// words.  Newlines may come between the parts.
//   or      := and (|| and)*
//   and     := not (&& not)*
//   not     := ! not | primary
//   primary := ( or ) | word | unop word | word binop word
bool Parser::par_cond() {
  mLex.consume();  // [[
  mLex.push_state(static_cast<LexFlags>(mLex.state() | LEX_COND));
  bool ok = par_cond_or();
  if (ok) {
    skip_newlines();
    if (at(TokType::TK_WORD) and mLex.peek().text == "]]") {
      mLex.pop_state();
      mLex.consume();
      return true;
    }
    ok = error_near(mLex.peek());
  }
  mLex.pop_state();
  return ok;
}

// cond := WC_COND(OR, skip) cond cond, left-associative; the header goes in
// front of the left side once the `||` is seen.
bool Parser::par_cond_or() {
  s::size_t start = mProg->prog.size();
  if (not par_cond_and()) return false;
  for (;;) {
    skip_newlines();
    if (not eat(TokType::TK_PIPEPIPE)) return true;
    skip_newlines();
    if (not par_cond_and()) return false;
    insert(start, make_wc_cmd(WC_COND, WC_COND_OR,
                              static_cast<WordCode>(mProg->prog.size() - start)));
  }
}

bool Parser::par_cond_and() {
  s::size_t start = mProg->prog.size();
  if (not par_cond_not()) return false;
  for (;;) {
    skip_newlines();
    if (not eat(TokType::TK_AMPAMP)) return true;
    skip_newlines();
    if (not par_cond_not()) return false;
    insert(start, make_wc_cmd(WC_COND, WC_COND_AND,
                              static_cast<WordCode>(mProg->prog.size() - start)));
  }
}

bool Parser::par_cond_not() {
  skip_newlines();
  if (not eat(TokType::TK_BANG)) return par_cond_primary();
  s::size_t head = emit(make_wc_cmd(WC_COND, WC_COND_NOT, 0));
  if (not par_cond_not()) return false;
  patch(head, make_wc_cmd(WC_COND, WC_COND_NOT, words_after(*mProg, head)));
  return true;
}

static bool is_cond_pattern_op(strv op) noexcept {
  return op == "=" or op == "==" or op == "!=" or op == "=~";
}

static bool is_cond_binary_op(strv op) noexcept {
  return is_cond_pattern_op(op) or op == "<" or op == ">"
      or op == "-nt" or op == "-ot" or op == "-ef" or op == "-eq" or op == "-ne"
      or op == "-lt" or op == "-le" or op == "-gt" or op == "-ge";
}

bool Parser::par_cond_primary() {
  skip_newlines();
  if (eat(TokType::TK_LPAREN)) {
    if (not par_cond_or()) return false;
    skip_newlines();
    return expect(TokType::TK_RPAREN);
  }
  if (not at_cond_word()) return error_near(mLex.peek());
  str w1 = mLex.peek().text;
  mLex.consume();
  // A single word: true when it is not empty.
  if (not at_cond_word()) {
    emit(make_wc_cmd(WC_COND, WC_COND_STR, 1));
    emit_str(s::move(w1));
    return true;
  }
  str w2 = mLex.peek().text;
  // `-x arg`, unless the second word is an operator (`-n = -n`).  An
  // unknown `-x` is reported when the condition runs, as zsh does.
  bool binary = is_cond_binary_op(w2)
             or ((w1.size() < 2 or w1[0] != '-') and w2.size() > 1 and w2[0] == '-');
  if (not binary) {
    if (w1.size() < 2 or w1[0] != '-') {
      error("parse error: condition expected: " + w1);
      return false;
    }
    mLex.consume();
    if (w1.size() > 2) {
      // `-NAME -x word`: zsh reads -x as an infix condition, which isn't
      // one here.
      if (w2.size() > 1 and w2[0] == '-' and at_cond_word()) return error_near(mLex.peek());
      // -NAME arg…: a condition a module defines, with its arguments.
      vec<str> args{s::move(w2)};
      for (;;) {
        // A pattern argument may start with a group: `-prefix 1 (f|ht)tp:`.
        if (at(TokType::TK_LPAREN)) {
          mLex.consume();
          auto pat = mLex.read_cond_pattern("(");
          if (not pat) return error_near(mLex.peek());
          args.push_back(s::move(*pat));
          continue;
        }
        if (not at_cond_word()) break;
        args.push_back(mLex.peek().text);
        mLex.consume();
      }
      emit(make_wc_cmd(WC_COND, WC_COND_MOD, static_cast<WordCode>(args.size() + 1)));
      emit_str(s::move(w1));
      for (auto& a : args) emit_str(s::move(a));
      return true;
    }
    emit(make_wc_cmd(WC_COND, WC_COND_UNARY, 2));
    emit_str(s::move(w1));
    emit_str(s::move(w2));
    return true;
  }
  mLex.consume();
  str w3;
  if (is_cond_pattern_op(w2)) {
    auto pat = mLex.read_cond_pattern();
    if (not pat or *pat == "]]") {
      if (pat) mLex.unread({TokType::TK_WORD, *pat, mLex.line()});
      return error_near(mLex.peek());
    }
    w3 = s::move(*pat);
  } else if (at(TokType::TK_LPAREN)) {
    // An arithmetic operand in parentheses: [[ $n -gt ((x + 4)) ]].
    mLex.consume();
    auto pat = mLex.read_cond_pattern("(");
    if (not pat) return error_near(mLex.peek());
    w3 = s::move(*pat);
  } else {
    if (not at_cond_word()) return error_near(mLex.peek());
    w3 = mLex.peek().text;
    mLex.consume();
  }
  emit(make_wc_cmd(WC_COND, WC_COND_BINARY, 3));
  emit_str(s::move(w2));
  emit_str(s::move(w1));
  emit_str(s::move(w3));
  return true;
}

// arith := WC_ARITH(0, 1) WC_STRING(expr)
bool Parser::par_arith() {
  mLex.consume();  // ((
  auto text = mLex.read_arith();
  if (not text) return error_near(mLex.peek());
  emit(make_wc_cmd(WC_ARITH, 0, 1));
  emit_str(s::move(*text));
  return true;
}

// Split a redirection token `text` (produced by the lexer) into an optional
// leading fd prefix (as an int, or -1 when absent) and the operator string.
// `2>&1` → (2, ">&"), `>>` → (-1, ">>"), `3<` → (3, "<").  The special form
// `2>&1` keeps `1` as a separate argument the caller reads next — this
// helper only detects the fd BEFORE the operator, not after.
static s::pair<int, str> split_redir_token(const str& text) {
  if (not text.empty() and text[0] == '{') {
    auto close = text.find('}');
    return {-1, text.substr(close + 1)};
  }
  s::size_t i = 0;
  while (i < text.size()
         and s::isdigit(static_cast<unsigned char>(text[i]))) ++i;
  if (i == 0) return {-1, text};
  if (i >= text.size()) return {-1, text};
  int fd = s::atoi(text.substr(0, i).c_str());
  return {fd, text.substr(i)};
}

static bool op_to_redir_type(strv op, RedirType& out) noexcept {
  if (op == "<")   { out = REDIR_READ;       return true; }
  if (op == ">")   { out = REDIR_WRITE;      return true; }
  if (op == ">>")  { out = REDIR_APPEND;     return true; }
  if (op == ">|")  { out = REDIR_CLOBBER;    return true; }
  if (op == "<>")  { out = REDIR_READWRITE;  return true; }
  if (op == "<<")  { out = REDIR_HEREDOC;    return true; }
  if (op == "<<-") { out = REDIR_HEREDOCD;   return true; }
  if (op == "<<<") { out = REDIR_HERESTR;    return true; }
  if (op == ">&")  { out = REDIR_MERGE;      return true; }
  if (op == "<&")  { out = REDIR_IN_DUP;     return true; }
  if (op == "&>")  { out = REDIR_MERGEOUT;   return true; }
  if (op == "&>>" or op == ">>&") { out = REDIR_APPEND_ERR; return true; }
  if (op == ">>|") { out = REDIR_APPEND_CLOBBER; return true; }
  if (op == "&>|" or op == ">&|") { out = REDIR_MERGEOUT_CLOBBER; return true; }
  if (op == "&>>|" or op == ">>&|") { out = REDIR_APPEND_ERR_CLOBBER; return true; }
  return false;
}

// Quote removal on a here-doc delimiter, reporting whether any quoting
// was there (which makes the body literal).  Inside '…' everything is
// literal; inside "…" a backslash escapes only " $ ` and \; elsewhere
// it escapes any character.  `'EOF'` → EOF, `E\\OF` → E\OF.
static str heredoc_unquote(strv raw, bool& quoted) {
  str out;
  quoted = false;
  char in = 0;   // the quote we are inside of, if any
  for (s::size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (in == '\'') {
      if (c == '\'') in = 0;
      else            out += c;
    } else if (c == '\\' and i + 1 < raw.size()
               and (in != '"' or s::strchr("\"$`\\", raw[i + 1]))) {
      quoted = true;
      out += raw[++i];
    } else if (in == '"') {
      if (c == '"') in = 0;
      else          out += c;
    } else if (c == '\'' or c == '"') {
      quoted = true;
      in = c;
    } else {
      out += c;
    }
  }
  return out;
}

bool Parser::par_redir() {
  // (A process substitution in its own place is a word: par_simple and
  // par_trailing_redirs take it before calling this.)
  LexToken op_tok = mLex.next_token();
  // `<(cmd` that the input ends inside.
  if (op_tok.text.size() >= 2 and op_tok.text[1] == '(') {
    error("parse error near `" + op_tok.text + "'");
    return false;
  }
  auto [fd, op] = split_redir_token(op_tok.text);
  RedirType type;
  if (not op_to_redir_type(op, type))
    return unsupported("redirection `" + op_tok.text + "'");
  // `{name}>…`: the name goes in front of the target.
  str varid;
  if (op_tok.text[0] == '{') {
    varid = op_tok.text.substr(1, op_tok.text.find('}') - 1);
    varid += '\0';
    if (type == REDIR_HEREDOC or type == REDIR_HEREDOCD or type == REDIR_MERGEOUT
        or type == REDIR_APPEND_ERR or type == REDIR_MERGEOUT_CLOBBER
        or type == REDIR_APPEND_ERR_CLOBBER) {
      error("parse error near `" + op_tok.text + "'");
      return false;
    }
  }

  // `>&-` and `<&-` close the fd, 1 and 0 by default.
  if (type == REDIR_MERGE or type == REDIR_IN_DUP) {
    const auto& tgt = mLex.peek();
    if (tgt.type == TokType::TK_WORD and tgt.text == "-") {
      mLex.consume();
      bool in = type == REDIR_IN_DUP;
      if (fd < 0) fd = in ? 0 : 1;
      emit(make_wc_redir(REDIR_CLOSE, fd, false, not varid.empty()));
      emit_str(varid + (in ? "<" : ">"));   // how it was written, for the text
      return true;
    }
  }

  // `< <(cmd)`, `2> >(cmd)`: a process substitution as the target joins
  // the given fd to the command's pipe.  `< =(cmd)` reads its file, and
  // `<<< <(cmd)` gives its path, both as a word the expansion runs.
  if (mLex.peek().type == TokType::TK_REDIR and is_procsubst(mLex.peek().text)) {
    const str& t = mLex.peek().text;
    if (type == REDIR_HERESTR or (type == REDIR_READ and t[0] == '=')) {
      emit(make_wc_redir(type, fd));
      emit_str(t);
      mLex.consume();
      return true;
    }
    bool out = type == REDIR_WRITE or type == REDIR_APPEND or type == REDIR_CLOBBER;
    bool in  = type == REDIR_READ;
    if (not (in or out) or (out and t[0] != '>') or (in and t[0] == '>')) {
      error("expected target after `" + op_tok.text + "'");
      return false;
    }
    emit(make_wc_redir(out ? REDIR_PROC_OUT : REDIR_PROC_IN, fd));
    emit_str(t.substr(2, t.size() - 3));
    mLex.consume();
    return true;
  }

  // Normal redirs: expect a WORD target.
  const auto& tgt = mLex.peek();
  if (tgt.type != TokType::TK_WORD and not is_reserved_word(tgt.type)) {
    error("expected target after `" + op_tok.text + "'");
    return false;
  }
  str target = tgt.text;
  mLex.consume();

  if (type == REDIR_HEREDOC or type == REDIR_HEREDOCD) {
    bool quoted = false;
    str delim = heredoc_unquote(target, quoted);
    mLex.register_heredoc(delim, type == REDIR_HEREDOCD, not quoted);
    // The lexer reads the body once it reaches the end of this line;
    // parse() fills the placeholder in.
    emit(make_wc_redir(type, fd, quoted));
    mPendingHeredocs.push_back(emit_str({}));
    return true;
  }

  emit(make_wc_redir(type, fd, false, not varid.empty()));
  emit_str(varid + target);
  return true;
}

}  // namespace jfsh
